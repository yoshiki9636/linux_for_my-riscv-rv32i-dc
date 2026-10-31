/* drivers/tty/serial/myuart.c */
#include <linux/serial_core.h>
#include <linux/console.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/kfifo.h>
#include <linux/timer.h>
#include <linux/tty_flip.h>
#include <linux/interrupt.h>
#include <linux/percpu.h>

#define UART_TX_OFFSET      0x00
#define UART_STATUS_OFFSET  0x04
#define UART_TX_READY       (1 << 0)  /* 1=FIFOフル, 0=送信可能 */
#define TIOCSER_TEMT  0x01


static void myuart_putc(char c)
{
    volatile uint32_t *status = (volatile uint32_t *)0xC000FC04;
    volatile uint32_t *tx     = (volatile uint32_t *)0xC000FC00;
    unsigned long flags;
    /* irq-off so the not-full check and the TX write stay paired */
    local_irq_save(flags);
    while (*status & 0x1);
    *tx = c;
    local_irq_restore(flags);
}

static void myuart_early_write(struct console *con,
                                const char *s, unsigned int n)
{
    unsigned int i;
    for (i = 0; i < n; i++) {
        myuart_putc(s[i]);
        if (s[i] == '\n')
            myuart_putc('\r');
    }
}

static int __init myuart_earlycon_setup(struct earlycon_device *dev,
                                         const char *options)
{
    dev->port.membase = (void __iomem *)0xC000FC00;
    dev->con->write = myuart_early_write;
    return 0;
}

EARLYCON_DECLARE(myuart, myuart_earlycon_setup);
OF_EARLYCON_DECLARE(myuart, "myvendor,myuart", myuart_earlycon_setup);

static void myuart_putchar(struct uart_port *port, unsigned char c)
{
    myuart_putc(c);
}

static void myuart_start_tx(struct uart_port *port)
{
    struct tty_port *tport = &port->state->port;
    unsigned char ch;
    while (kfifo_get(&tport->xmit_fifo, &ch)) {
        myuart_putchar(port, ch);
        port->icount.tx++;
    }
}

static unsigned int myuart_tx_empty(struct uart_port *port)
{
    volatile uint32_t *status = (volatile uint32_t *)0xC000FC04;
    return (*status & 0x1) ? 0 : TIOCSER_TEMT;
}

static void myuart_set_mctrl(struct uart_port *port, unsigned int mctrl) {}
static unsigned int myuart_get_mctrl(struct uart_port *port) { return 0; }
static void myuart_stop_tx(struct uart_port *port) {}
static void myuart_stop_rx(struct uart_port *port) {}

/* FPGA (2026-07-19): polled console INPUT via the FPGA UART RX latch.
 * Hardware (io_uart_out.v): reading 0xC000FC0C returns
 *   [7:0] last received byte, [8] data-ready, [9] overrun,
 * and the READ ITSELF clears the ready/overrun flags (single-char
 * latch, no FIFO).  0xC000FC10 bit0=1 disables the monitor's hardware
 * echo-back (the shell does its own echo).  The monitor's LETTER
 * commands (g/w/r/t/s/p/i/j/z/l/m) are gated off while the CPU runs
 * (uart_rec_char.v mon_en), so ordinary shell text lands here instead
 * of being misparsed as monitor commands.  Ctrl-c (0x03) is left
 * ungated on purpose - it's the normal way back to the monitor while
 * Linux is running and is never sent by ordinary typed text, so it
 * still stops the CPU immediately as before (does not reach this RX
 * latch).  Paste can still drop characters (single-byte latch).
 *
 * FPGA (2026-10-30, AS): RX is interrupt driven when the DT node has an
 * interrupt (riscv-intc hwirq 11 = machine external interrupt).  Each
 * received byte pulses ext_uart_interrpt_1shot; interrupter.v latches it in
 * SYS_INT_STATUS bit0 (if SYS_INT_ENABLE bit0 and mie.MEIE are set), which
 * drives mip.MEIP until software writes 0 to that bit.  Without an
 * interrupt in the DT the driver falls back to the old 10 ms timer poll. */
#define MYUART_RX_REG     ((volatile uint32_t *)0xC000FC0C)
#define MYUART_RXEC_REG   ((volatile uint32_t *)0xC000FC10)
#define MYUART_INT_EN_REG ((volatile uint32_t *)0xC000FA00) /* bit0 RX, bit1 ext pin */
#define MYUART_INT_ST_REG ((volatile uint32_t *)0xC000FA04) /* write 0 to clear a bit */

static struct timer_list myuart_rx_timer;
static struct uart_port *myuart_rx_port;
static int myuart_rx_irq;              /* 0 = timer poll */
static DEFINE_PER_CPU(struct uart_port *, myuart_irq_cookie);

static void myuart_rx_read(struct uart_port *port)
{
    uint32_t v = *MYUART_RX_REG;   /* read consumes data + clears flags */

    if (port && (v & 0x100)) {
        struct tty_port *tport = &port->state->port;

        if (v & 0x200)
            port->icount.overrun++;
        port->icount.rx++;
        tty_insert_flip_char(tport, v & 0xff, TTY_NORMAL);
        tty_flip_buffer_push(tport);
    }
}

static void myuart_rx_poll(struct timer_list *t)
{
    struct uart_port *port = READ_ONCE(myuart_rx_port);

    myuart_rx_read(port);
    if (port)
        mod_timer(&myuart_rx_timer, jiffies + msecs_to_jiffies(10));
}

static irqreturn_t myuart_rx_interrupt(int irq, void *dev_id)
{
    /* Clear the latched RX status before reading the data, so a byte that
     * arrives after the read raises a new interrupt.  Bit1 (ext pin) is
     * written as 1, which leaves it alone. */
    *MYUART_INT_ST_REG = 0x2;
    myuart_rx_read(READ_ONCE(myuart_rx_port));
    return IRQ_HANDLED;
}

static int  myuart_startup(struct uart_port *port)
{
    *MYUART_RXEC_REG = 1;          /* no hardware echo-back */
    (void)*MYUART_RX_REG;          /* flush any stale byte/flags */
    WRITE_ONCE(myuart_rx_port, port);
    if (myuart_rx_irq) {
        *MYUART_INT_ST_REG = 0x2;
        *MYUART_INT_EN_REG = 1;
        enable_percpu_irq(myuart_rx_irq, IRQ_TYPE_NONE);   /* sets mie.MEIE */
    } else {
        mod_timer(&myuart_rx_timer, jiffies + msecs_to_jiffies(10));
    }
    return 0;
}

static void myuart_shutdown(struct uart_port *port)
{
    if (myuart_rx_irq) {
        disable_percpu_irq(myuart_rx_irq);
        *MYUART_INT_EN_REG = 0;
    }
    WRITE_ONCE(myuart_rx_port, NULL);
    timer_delete_sync(&myuart_rx_timer);
}
static void myuart_set_termios(struct uart_port *port,
                               struct ktermios *new,
                               const struct ktermios *old) {}
static const char *myuart_type(struct uart_port *port) { return "myuart"; }
static void myuart_release_port(struct uart_port *port) {}
static int  myuart_request_port(struct uart_port *port) { return 0; }
/*
 * FPGA (2026-10-27): the port type must NOT be PORT_UNKNOWN - serial_core's
 * uart_port_startup() returns 1 for PORT_UNKNOWN without calling ->startup,
 * and uart_startup() then sets TTY_IO_ERROR, so every write()/read() on
 * /dev/console returned -EIO (PID1's stderr writev, kernel#843) and the RX
 * poll timer was never armed. No upstream PORT_* id fits; 124 is unused.
 */
#define PORT_MYUART 124

static void myuart_config_port(struct uart_port *port, int flags)
{
    if (flags & UART_CONFIG_TYPE)
        port->type = PORT_MYUART;
}
static int myuart_verify_port(struct uart_port *port,
                              struct serial_struct *ser) { return 0; }

static const struct uart_ops myuart_ops = {
    .tx_empty     = myuart_tx_empty,
    .set_mctrl    = myuart_set_mctrl,
    .get_mctrl    = myuart_get_mctrl,
    .stop_tx      = myuart_stop_tx,
    .start_tx     = myuart_start_tx,
    .stop_rx      = myuart_stop_rx,
    .startup      = myuart_startup,
    .shutdown     = myuart_shutdown,
    .set_termios  = myuart_set_termios,
    .type         = myuart_type,
    .release_port = myuart_release_port,
    .request_port = myuart_request_port,
    .config_port  = myuart_config_port,
    .verify_port  = myuart_verify_port,
};

#define MYUART_MAX_PORTS 1
static struct uart_port myuart_ports[MYUART_MAX_PORTS];
static struct uart_driver myuart_driver;

static void myuart_console_write(struct console *co,
                                 const char *s, unsigned int n)
{
    unsigned long flags;
    struct uart_port *port;
    local_irq_save(flags);
    port = &myuart_ports[co->index];
    uart_console_write(port, s, n, myuart_putchar);
    local_irq_restore(flags);
}

static int myuart_console_setup(struct console *co, char *options)
{
    //if (co->index < 0 || co->index >= MYUART_MAX_PORTS)
        //return -ENODEV;
    //if (!myuart_ports[co->index].membase)
        //return -ENODEV;
	myuart_ports[0].membase = (void __iomem *)0xC000FC00;
    return 0;
}

    //.name   = "ttyMY",
    //.index  = -1,
struct console myuart_console = {
    .name   = "ttyMY",
    .write  = myuart_console_write,
    .device = uart_console_device,
    .setup  = myuart_console_setup,
    .flags  = CON_PRINTBUFFER | CON_ENABLED,
    .index  = 0,
    .data   = &myuart_driver,
};

static struct uart_driver myuart_driver = {
    .owner       = THIS_MODULE,
    .driver_name = "myuart",
    .dev_name    = "ttyMY",
    .nr          = MYUART_MAX_PORTS,
    .cons        = &myuart_console,
};

static int myuart_probe(struct platform_device *pdev)
{
    struct uart_port *port;
    struct resource  *res;
    int ret, irq;

    if (pdev->id >= MYUART_MAX_PORTS)
        return -EINVAL;

    port = &myuart_ports[pdev->id < 0 ? 0 : pdev->id];
    memset(port, 0, sizeof(*port));

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res)
        return -ENODEV;

    port->membase = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(port->membase))
        return PTR_ERR(port->membase);

    port->mapbase = res->start;
    port->ops     = &myuart_ops;
    port->type    = PORT_MYUART;
    port->dev     = &pdev->dev;
    port->line    = pdev->id < 0 ? 0 : pdev->id;
    port->iotype  = UPIO_MEM;
    port->flags   = UPF_BOOT_AUTOCONF;

    timer_setup(&myuart_rx_timer, myuart_rx_poll, 0);

    /* riscv-intc interrupts are per-CPU (percpu_devid) */
    irq = platform_get_irq_optional(pdev, 0);
    if (irq > 0) {
        ret = request_percpu_irq(irq, myuart_rx_interrupt, "myuart",
                                 &myuart_irq_cookie);
        if (ret)
            dev_warn(&pdev->dev, "irq %d request failed (%d), polling RX\n",
                     irq, ret);
        else
            myuart_rx_irq = port->irq = irq;
    }
    dev_info(&pdev->dev, "RX %s\n", myuart_rx_irq ? "interrupt driven" : "polled");

    ret = uart_add_one_port(&myuart_driver, port);
    if (ret)
        return ret;

    platform_set_drvdata(pdev, port);
    return 0;
}

static void myuart_remove(struct platform_device *pdev)
{
    struct uart_port *port = platform_get_drvdata(pdev);
    uart_remove_one_port(&myuart_driver, port);
}

static const struct of_device_id myuart_of_match[] = {
    { .compatible = "myvendor,myuart" },
    {}
};
MODULE_DEVICE_TABLE(of, myuart_of_match);

static struct platform_driver myuart_platform_driver = {
    .probe  = myuart_probe,
    .remove = myuart_remove,
    .driver = {
        .name           = "myuart",
        .of_match_table = myuart_of_match,
    },
};

static int __init myuart_console_init(void)
{
    /* FPGA (2026-10-29, #881): init/main.c is back to upstream and no longer
     * registers myuart_console by hand before initcalls, so do it here. */
    register_console(&myuart_console);
    return 0;
}


static int __init myuart_init(void)
{
    int ret;

    ret = uart_register_driver(&myuart_driver);
    if (ret)
        return ret;
    ret = platform_driver_register(&myuart_platform_driver);
    if (ret) {
        uart_unregister_driver(&myuart_driver);
    }
    return ret;
}

static void __exit myuart_exit(void)
{
    platform_driver_unregister(&myuart_platform_driver);
    uart_unregister_driver(&myuart_driver);
}

console_initcall(myuart_console_init);
//console_initcall(myuart_init);

module_init(myuart_init);
module_exit(myuart_exit);

MODULE_DESCRIPTION("My Custom FPGA UART Driver");
MODULE_LICENSE("GPL");
