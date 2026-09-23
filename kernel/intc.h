// interrupt controller abstraction (intc.c) and its backends.

// IPI reasons (bits in the per-hart mailbox).
#define IPI_KICK 1  // a vcpu running on that hart must exit to re-evaluate
#define IPI_TLBFLUSH 2 // flush the TLB and acknowledge (kstack.c)
#define IPI_RESCHED 4  // something with less virtual time is waiting there

// IMSIC interrupt identity used for IPIs (device irqs are 1..95).
#define IMSIC_IPI_ID 100

void intc_init(void);
void intc_inithart(void);
int intc_claim(void);
void intc_complete(int irq);
void intc_ipi(int hart, uint64 reason);
uint64 ipi_handle(void);
extern volatile uint64 ipi_count[];

// plic.c
void plicinit(void);
void plicinithart(void);
int plic_claim(void);
void plic_complete(int);

// aplic.c
void aplic_minit(void); // machine mode, hart 0: root domain
void aplic_init(int msi);
void aplic_inithart(void);
int aplic_claim(void);
void aplic_complete(int);

// imsic.c
void imsic_init(void);
void imsic_inithart(void);
int imsic_claim(void);
void imsic_ipi(int hart);
uint64 imsic_file_addr(int hart, int guest);
