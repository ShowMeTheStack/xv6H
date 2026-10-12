/*
 * The revision protocol of xv6H, as a Promela model.
 *
 * One machine of two harts, a fact f that the host revises (a fresh value
 * each time), two hosts A and B between which the machine may cross, an
 * announcement that may be lost, and the alias: the host serves an access
 * keyed by f under the old value until the guest has acknowledged.
 *
 * The protocol as implemented: the announcement is one software interrupt
 * per hart; the host repeats it while no hart has read the description; the
 * first hart to read the description takes the global part and acknowledges
 * (two steps: the read, then the ack, with the guest's own work between);
 * every other hart brings its own (controller, timer) state up on its own
 * announcement. The host asks one thing at a time, and a machine crosses
 * only between revisions (both bounded waits for the ack).
 *
 * Checked:
 *   SAFETY    every access the guest makes keyed by f is served correctly
 *             (the guest's belief matches what the host serves), and the
 *             generation is only ever the host's (gen_owner);
 *   adopted   every revision is eventually acknowledged;
 *   caught_up every hart eventually brings its own state up to the revision.
 *
 *   spin -a [-D...] revision.pml
 *   gcc -O2 -DSAFETY -o pan pan.c; ./pan                    (safety)
 *   gcc -O2 -o pan pan.c; ./pan -a -f -N adopted            (liveness, weak fairness;
 *                                                            -DNFAIR=3 with TICKCHECK)
 *                         ./pan -a -f -N caught_up
 *
 * Variants (each is a version of the host or guest that existed):
 *   -DCROSS_MID     the machine may cross in the middle of a revision, the
 *                   destination asking its own (the host before defect 22):
 *                   SAFETY fails -- the alias of a revision the guest has not
 *                   read is not in the stream, so the guest's numbering is not
 *                   answered at the destination.
 *   -DCARRY_ALIAS   with CROSS_MID: the destination also serves what the guest
 *                   believed before the source's revision. SAFETY still fails:
 *                   the guest's ack of the source's generation, given after the
 *                   crossing, ends the destination's revision while the guest
 *                   is on the source's description (defect 20).
 *   -DACK_NAMES_GEN the ack carries the generation it acknowledges and a stale
 *                   one is refused (defect 20's fix). With CROSS_MID and
 *                   CARRY_ALIAS, SAFETY then holds.
 *   -DNOREPEAT      the announcement is never repeated (the first host):
 *                   adopted fails, a lost announcement is never taken up.
 *   -DTICKCHECK     a hart checks at every tick whether it owes the per-hart
 *                   part of a revision another hart took (defect 21's fix).
 *                   Without it caught_up fails: a hart whose announcement was
 *                   lost after the first hart had read the description is never
 *                   told again, the host having stopped repeating and the ack
 *                   having been given.
 */
#define NH 2            /* harts of the guest */
#define REVS 3          /* revisions the host makes (crossings count) */
#define MAXLOSS 2       /* announcements the channel may lose */

byte  gen = 0;          /* the host's generation (the host's alone) */
byte  fact = 0;         /* the fact's value the host now has */
byte  oldfact = 0;      /* the value served through the alias until the ack */
byte  nextval = 1;      /* the next fresh value of the fact */
bool  pending = false;  /* asked and not yet acknowledged */
bool  described = false;/* the guest has read the new description */
byte  acked = 0;        /* the generation the guest last acknowledged */
byte  stale = 0;        /* acks refused as naming an older generation */
byte  host = 0;         /* which host runs the machine: 0 = A, 1 = B */
byte  losses = 0;
byte  revs = 0;

/* the guest's state */
byte  belief = 0;       /* the value of f the guest is using */
byte  gen_hart[NH];     /* which generation each hart has brought its own state up to */
byte  seen = 0;         /* the generation the guest has read (revision_global) */
chan  announce[NH] = [1] of { byte };   /* one pending notification per hart */

inline serve(access_value)
{
    /* the host serves an access keyed by f: the new value, or the old one while pending */
    assert(access_value == fact || (pending && access_value == oldfact));
}

inline announce_all()
{
    h = 0;
    do
    :: h < NH -> if :: nfull(announce[h]) -> announce[h] ! gen :: else -> skip fi; h++
    :: h >= NH -> break
    od
}

active proctype Host()
{
    byte h;
end_h:
    do
    :: atomic { revs < REVS && !pending ->
        /* a revision: the fact changes, the old value stays served until the ack */
            oldfact = fact;
            fact = nextval; nextval++;
            pending = true; described = false;
            gen++;
            revs++;
            announce_all()
        }
#ifndef NOREPEAT
    :: atomic { pending && !described ->
        /* at-least-once: repeated while no hart has read the description */
            announce_all() }
#endif
    :: !pending && revs < REVS ->
        /* a crossing to the other host between revisions: the machine keeps its facts */
        host = 1 - host
#ifdef CROSS_MID
    :: atomic { pending && revs < REVS ->
        /* a crossing in the middle of a revision: the destination asks its own.
         * The stream carries the description the machine is on (the source's
         * new value), which the destination aliases; what the guest believed
         * before the source's revision is not in the stream ... */
            host = 1 - host;
#ifdef CARRY_ALIAS
            if
            :: described -> oldfact = fact   /* ... unless carried: the guest may be on either */
            :: else -> skip
            fi;
#else
            oldfact = fact;
#endif
            fact = nextval; nextval++;
            gen++;
            revs++;
            described = false;
            announce_all()
        }
#endif
    :: revs >= REVS && !pending -> break
    od
}

active proctype Channel()
{
    /* the announcement can be lost: a bounded number of times */
    byte h;
end_c:
    do
    :: losses < MAXLOSS ->
        if
        :: nempty(announce[0]) -> announce[0] ? h; losses++
        :: nempty(announce[1]) -> announce[1] ? h; losses++
        fi
    od
}

inline ack(g)
{
#ifdef ACK_NAMES_GEN
    if
    :: g == gen -> acked = g; pending = false
    :: else -> stale++          /* refused: the announcement stands */
    fi
#else
    acked = g; pending = false  /* the first version: any ack ends the pending revision */
#endif
}

proctype Hart(byte me)
{
    byte g;
    bool mine;
end_g:
    do
    :: announce[me] ? g ->
        /* the trap that announces */
        atomic {
            if
            :: seen != gen ->
                /* describe(): the first hart here reads the description and
                 * takes the global part ... */
                seen = gen;
                described = true;
                belief = fact;
                gen_hart[me] = seen;
                g = seen;
                mine = true
            :: else ->
                /* another hart took the global part: only this hart's own state */
                gen_hart[me] = seen;
                mine = false
            fi
        };
        if
        :: mine ->
            /* ... and then, its own work done, acknowledges what it read */
            atomic { ack(g) }
        :: else -> skip
        fi
    :: empty(announce[me]) ->
        /* an ordinary access keyed by f, with whatever the guest believes;
         * a pending interrupt is taken before the next instruction */
        serve(belief)
    od
}

#ifdef TICKCHECK
proctype Tick(byte me)
{
    /* the timer interrupt: the hart checks whether it owes the per-hart part
     * of a revision another hart took (machine_revise_owed) */
end_t:
    do
    :: atomic { gen_hart[me] != seen -> gen_hart[me] = seen }   /* -DNFAIR=3 for pan: six processes */
    od
}
#endif

init {
    atomic {
        run Hart(0); run Hart(1);
#ifdef TICKCHECK
        run Tick(0); run Tick(1);
#endif
    }
}

/* the generation is the host's: the guest never reads one the host did not write */
ltl gen_owner { [] (seen <= gen) }
/* every revision is eventually acknowledged */
ltl adopted   { [] (pending -> <> !pending) }
/* every hart eventually brings its own state up to the revision */
ltl caught_up { [] ((gen_hart[0] != gen || gen_hart[1] != gen) -> <> (gen_hart[0] == gen && gen_hart[1] == gen)) }
