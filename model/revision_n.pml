/*
 * The revision protocol of xv6H with NH harts and NF facts.
 *
 * revision.pml models one fact and two harts. Here the host may revise any of NF facts, and
 * a fact asked while another revision is pending is owed: its bit joins the next revision's
 * what-set (the host asks one thing at a time; kvmtool's revise__ask unions owed bits). The
 * guest reads the description once per generation; any hart may be first. Acks name the
 * generation (defect 20's fix) and harts check at every tick for a per-hart part they owe
 * (defect 21's fix). Announcements may be lost, a bounded number of times.
 *
 * Checked:
 *   SAFETY    every access keyed by any fact is served correctly: the guest's belief for that
 *             fact is the host's value, or the old value while that fact's revision is pending;
 *             the generation is only ever the host's;
 *   adopted   every revision is eventually acknowledged;
 *   no_owed   every fact asked for is eventually in an acknowledged revision;
 *   caught_up every hart eventually reaches the host's generation.
 *
 * The rate. Fact 0 is the counter's rate, which cannot be aliased: the host
 * serves one rate at a time (`served'), and every time read must be converted with the rate the
 * guest is using (served == belief[0]). The protocol as fixed serves the new rate from the
 * guest's re-base, which calls MACH call 7 (rate_now) in the same step (Linux re-bases every CPU
 * inside stop_machine, with no time read in between), or from the ack if the guest never calls
 * it. Variants: -DRATE_AT_ASK serves it from the ask (defect 49, before its fix); -DNO_CALL7
 * serves it only from the ack. Both must fail SAFETY.
 *
 *   spin -a -DNH=3 -DNF=2 revision_n.pml; gcc -O2 -DSAFETY -o pan pan.c; ./pan
 *   gcc -O2 -DNFAIR=6 -o pan pan.c; ./pan -a -f -N adopted     (and no_owed, caught_up)
 */
#ifndef NH
#define NH 3
#endif
#ifndef NF
#define NF 2
#endif
#ifndef ASKS
#define ASKS 3          /* facts the host asks to change, in all */
#endif
#define MAXLOSS 2

byte gen = 0;
byte fact[NF];          /* the host's value of each fact */
byte oldfact[NF];       /* served through the alias while that fact is in the pending revision */
byte served = 0;        /* the counter's rate the host serves now (fact 0) */
byte nextval = 1;
byte what = 0;          /* bits of the pending revision */
byte owed = 0;          /* bits asked while a revision was pending */
bool pending = false;
bool described = false;
byte asks = 0;
byte losses = 0;
byte acked = 0;

byte belief[NF];
byte gen_hart[NH];
byte seen = 0;
chan announce[NH] = [1] of { byte };

inline announce_all()
{
    h = 0;
    do
    :: h < NH -> if :: nfull(announce[h]) -> announce[h] ! gen :: else -> skip fi; h++
    :: h >= NH -> break
    od
}

/* a revision of every fact in bits w: fresh values, the old ones aliased until the ack */
inline revise(w)
{
    i = 0;
    do
    :: i < NF -> if :: (w >> i) & 1 -> oldfact[i] = fact[i]; fact[i] = nextval; nextval++ :: else -> skip fi; i++
    :: i >= NF -> break
    od;
#ifdef RATE_AT_ASK
    if :: w & 1 -> served = fact[0] :: else -> skip fi;
#endif
    what = w; pending = true; described = false; gen++;
    announce_all()
}

active proctype Host()
{
    byte h, i, f;
end_h:
    do
    :: atomic { asks < ASKS ->
        /* the host wants fact f changed */
        select(f : 0 .. NF - 1);
        asks++;
        if
        :: !pending -> revise(1 << f)
        :: else -> owed = owed | (1 << f)      /* one at a time: owed to the next revision */
        fi }
    :: atomic { !pending && owed ->
        /* what was owed goes out together */
        f = owed; owed = 0; revise(f) }
    :: atomic { pending && !described -> announce_all() }
    :: asks >= ASKS && !pending && !owed -> break
    od
}

active proctype Channel()
{
    byte h, x;
end_c:
    do
    :: losses < MAXLOSS ->
        select(h : 0 .. NH - 1);
        if
        :: nempty(announce[h]) -> announce[h] ? x; losses++
        :: empty(announce[h]) -> skip
        fi
    od
}

inline ack(g)
{
    if
    :: g == gen -> acked = g; pending = false; what = 0; served = fact[0]   /* at the ack, at the latest */
    :: else -> skip                 /* a stale generation is refused */
    fi
}

inline serve_all()
{
    i = 0;
    do
    :: i < NF ->
        assert(belief[i] == fact[i] || (pending && ((what >> i) & 1) && belief[i] == oldfact[i]));
        i++
    :: i >= NF -> break
    od;
    assert(served == belief[0])       /* a time read converted with the rate it is counted at */
}

proctype Hart(byte me)
{
    byte g, i;
    bool mine;
end_g:
    do
    :: announce[me] ? g ->
        atomic {
            if
            :: seen != gen ->
                seen = gen; described = true;
                i = 0;
                do
                :: i < NF -> belief[i] = fact[i]; i++
                :: i >= NF -> break
                od;
#ifndef NO_CALL7
                served = fact[0];          /* the re-base calls rate_now (MACH call 7) */
#endif
                gen_hart[me] = seen; g = seen; mine = true
            :: else -> gen_hart[me] = seen; mine = false
            fi
        };
        if
        :: mine -> atomic { ack(g) }
        :: else -> skip
        fi
    :: empty(announce[me]) -> atomic { serve_all() }
    od
}

proctype Tick(byte me)
{
end_t:
    do
    :: atomic { gen_hart[me] != seen -> gen_hart[me] = seen }
    od
}

init {
    byte k = 0;
    atomic {
        do
        :: k < NH -> run Hart(k); run Tick(k); k++
        :: k >= NH -> break
        od
    }
}

ltl gen_owner { [] (seen <= gen) }
ltl adopted   { [] (pending -> <> !pending) }
ltl no_owed   { [] (owed != 0 -> <> (owed == 0)) }
ltl caught_up { [] (gen_hart[NH - 1] != gen -> <> (gen_hart[NH - 1] == gen)) }
