#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/kdev_t.h>
#include <linux/uaccess.h>
#include <linux/errno.h>
#include <linux/device.h>

#include <linux/io.h> //iowrite ioread
#include <linux/slab.h>//kmalloc kfree
#include <linux/platform_device.h>//platform driver
#include <linux/of.h>//of_match_table
#include <linux/ioport.h>//ioremap

#include <linux/interrupt.h> //irqreturn_t, request_irq

// REGISTER CONSTANTS
#define XIL_AXI_TIMER_TCSR_OFFSET	0x0 // timer0 control and status register
#define XIL_AXI_TIMER_TLR_OFFSET	0x4 // timer0 load register
#define XIL_AXI_TIMER_TCR_OFFSET	0x8 // timer0 counter tegister

#define XIL_AXI_TIMER_TCSR1_OFFSET	0x10 // timer1 CSR
#define XIL_AXI_TIMER_TLR1_OFFSET 	0x14 // timer1 LR
#define XIL_AXI_TIMER_TCR1_OFFSET 	0x18 // timer1 CR

//TODO: define timer1 TCSR, TLR, TCR registers
//TODO: implement bitmask for cascade mode operation of timer (bit 11 of TCSR)

#define XIL_AXI_TIMER_CSR_CASC_MASK		0x00000800
#define XIL_AXI_TIMER_CSR_ENABLE_ALL_MASK	0x00000400
#define XIL_AXI_TIMER_CSR_ENABLE_PWM_MASK	0x00000200
#define XIL_AXI_TIMER_CSR_INT_OCCURED_MASK 	0x00000100
#define XIL_AXI_TIMER_CSR_ENABLE_TMR_MASK 	0x00000080
#define XIL_AXI_TIMER_CSR_ENABLE_INT_MASK 	0x00000040
#define XIL_AXI_TIMER_CSR_LOAD_MASK 		0x00000020
#define XIL_AXI_TIMER_CSR_AUTO_RELOAD_MASK 	0x00000010
#define XIL_AXI_TIMER_CSR_EXT_CAPTURE_MASK 	0x00000008
#define XIL_AXI_TIMER_CSR_EXT_GENERATE_MASK 	0x00000004
#define XIL_AXI_TIMER_CSR_DOWN_COUNT_MASK 	0x00000002
#define XIL_AXI_TIMER_CSR_CAPTURE_MODE_MASK 	0x00000001

#define BUFF_SIZE 20
#define DRIVER_NAME "timer"
#define DEVICE_NAME "xilaxitimer"

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR ("Xilinx");
MODULE_DESCRIPTION("Test Driver for Zynq PL AXI Timer.");
MODULE_ALIAS("custom:xilaxitimer");

struct timer_info {
	unsigned long mem_start;
	unsigned long mem_end;
	void __iomem *base_addr;
	int irq_num;
};

dev_t my_dev_id;
static struct class *my_class;
static struct device *my_device;
static struct cdev *my_cdev;
static struct timer_info *tp = NULL;
struct fasync_struct *async_queue;

int endRead = 0;
// cnt the interrupts
static int i_cnt = 0;
// mode of operation;
static char mode = 0;
unsigned long long int initial_value = 300000;
int en_init = 1;

static irqreturn_t xilaxitimer_isr(int irq,void*dev_id);
static void setup_timer(unsigned long long int milliseconds);
static void start_timer(void);
static void stop_timer(void);
static int timer_probe(struct platform_device *pdev);
static int timer_remove(struct platform_device *pdev);
int timer_open(struct inode *pinode, struct file *pfile);
int timer_close(struct inode *pinode, struct file *pfile);
ssize_t timer_read(struct file *pfile, char __user *buffer, size_t length, loff_t *offset);
ssize_t timer_write(struct file *pfile, const char __user *buffer, size_t length, loff_t *offset);
static int __init timer_init(void);
static void __exit timer_exit(void);
static int lifo_fasync(int fd,struct file *file, int mode);

static int lifo_fasync(int fd,struct file *file, int mode)
{
    return fasync_helper(fd, file, mode, &async_queue);
}

struct file_operations my_fops =
{
	.owner = THIS_MODULE,
	.open = timer_open,
	.read = timer_read,
	.write = timer_write,
	.release = timer_close,
	.fasync = lifo_fasync,
};
//axi-timer
static struct of_device_id timer_of_match[] = {
	{ .compatible = "axi_timer", },
	{ /* end of list */ },
};

static struct platform_driver timer_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table	= timer_of_match,
	},
	.probe		= timer_probe,
	.remove		= timer_remove,     
};


MODULE_DEVICE_TABLE(of, timer_of_match);

//***************************************************
// INTERRUPT SERVICE ROUTINE (HANDLER)

static irqreturn_t xilaxitimer_isr(int irq,void*dev_id)		
{      
	unsigned int data = 0;

	// Check Timer Counter Value
	data = ioread32(tp->base_addr + XIL_AXI_TIMER_TCR_OFFSET);
	printk(KERN_INFO "xilaxitimer_isr: Interrupt %d occurred !\n",i_cnt);

	// Clear Interrupt
	data = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR_OFFSET);
	iowrite32(data | XIL_AXI_TIMER_CSR_INT_OCCURED_MASK,
			tp->base_addr + XIL_AXI_TIMER_TCSR_OFFSET);

    // TODO: if interrupt, use asynchronous signal to stop program
	printk(KERN_NOTICE "xilaxitimer_isr: Interrupt has occurred. Disabling timer\n");
	data = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR_OFFSET);
	iowrite32(data & ~(XIL_AXI_TIMER_CSR_ENABLE_TMR_MASK), tp->base_addr + XIL_AXI_TIMER_TCSR_OFFSET);
	iowrite32(data & ~(XIL_AXI_TIMER_CSR_ENABLE_TMR_MASK), tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);

	printk(KERN_NOTICE "xilaxitimer_isr: Asynchronous signal! Shutdown application!\n");
	kill_fasync(&async_queue, SIGIO, POLL_IN);

	return IRQ_HANDLED;
}
//***************************************************
//HELPER FUNCTION THAT RESETS TIMER WITH PERIOD IN MILISECONDS

//TODO: use this function to initialize timer so it starts countint and use it only when OPEN is used
static void setup_timer(unsigned long long int milliseconds)
{
	unsigned int timer_load0;
	unsigned int timer_load1;
	unsigned int data = 0;

	u64 cnt00 = 0;
	u64 cnt1 = 0;
	u64 cnt = 0;
 
	timer_load0 = (unsigned int) (0x00000000ffffffff & ((initial_value - milliseconds)*100000));
	timer_load1 = (unsigned int) ((0xffffffff00000000 & ((initial_value - milliseconds)*100000)) >> 32);

	printk(KERN_INFO "setup_timer: miliseconds %llu", milliseconds);
	printk(KERN_INFO "setup_timer: timer_load0 %u", timer_load0);
	printk(KERN_INFO "setup_timer: timer_load1 %u", timer_load1);

	// Set initial value in load register0
	iowrite32(timer_load0, tp->base_addr + XIL_AXI_TIMER_TLR_OFFSET);
	// Set initial value in load register1
	iowrite32(timer_load1, tp->base_addr + XIL_AXI_TIMER_TLR1_OFFSET);

	// Load initial value into counter from load register0
	data = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR_OFFSET);
	// set mask for loading and load values0
	iowrite32(data | XIL_AXI_TIMER_CSR_LOAD_MASK,
			tp->base_addr + XIL_AXI_TIMER_TCSR_OFFSET);
	// read back loaded values and disable loading with !XIL_AXI_TIMER_CSR_LOAD_MASK
	data = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR_OFFSET);
	iowrite32(data & ~(XIL_AXI_TIMER_CSR_LOAD_MASK),
			tp->base_addr + XIL_AXI_TIMER_TCSR_OFFSET);

	// Load initial value into counter1 from load register1
	data = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);
	// set mask for loading and load values
	iowrite32(data | XIL_AXI_TIMER_CSR_LOAD_MASK,
			tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);
	// read back loaded values and disable loading with !XIL_AXI_TIMER_CSR_LOAD_MASK
	data = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);
	iowrite32(data & ~(XIL_AXI_TIMER_CSR_LOAD_MASK),
			tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);

	cnt00 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCR_OFFSET);
	cnt1 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCR1_OFFSET);
	cnt = (cnt1 << 32) | cnt00;

	//TODO: use length?
	printk(KERN_INFO "timer_setup_end: cnt0 = %llu\n", cnt00);
	printk(KERN_INFO "timer_setup_end: cnt1 = %llu\n", cnt1);
	printk(KERN_INFO "timer_setup_end: cnt = %llu\n", cnt);

}

//***************************************************
//HELPER FUNCTION THAT STARTS TIMER
static void start_timer(void) {
	unsigned int data = 0;
	//if(en_init) en_init = 0;
    	// Enable interrupts, count_down, autoreload and cascade mode, rest should be zero
	iowrite32(XIL_AXI_TIMER_CSR_DOWN_COUNT_MASK | XIL_AXI_TIMER_CSR_ENABLE_INT_MASK | XIL_AXI_TIMER_CSR_AUTO_RELOAD_MASK | XIL_AXI_TIMER_CSR_CASC_MASK,
			tp->base_addr + XIL_AXI_TIMER_TCSR_OFFSET);

	// Enable interrupts1, count_down and autoreload1, rest should be zero
	iowrite32(XIL_AXI_TIMER_CSR_DOWN_COUNT_MASK | XIL_AXI_TIMER_CSR_ENABLE_INT_MASK | XIL_AXI_TIMER_CSR_AUTO_RELOAD_MASK,
			tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);

	// TODO: do we need interrupt from TCSR0?
	// Start Timer bz setting enable signal
	data = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR_OFFSET);
	iowrite32(data | XIL_AXI_TIMER_CSR_ENABLE_TMR_MASK,
			tp->base_addr + XIL_AXI_TIMER_TCSR_OFFSET);

	// Start Timer bz setting enable signal
	data = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);
	iowrite32(data | XIL_AXI_TIMER_CSR_ENABLE_TMR_MASK,
			tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);
}

//***************************************************
//HELPER FUNCTION THAT STOPS TIMER
static void stop_timer(void) {
    unsigned int data = 0;
    
    // Enable interrupts, count_down, autoreload and cascade mode, rest should be zero
	iowrite32(XIL_AXI_TIMER_CSR_DOWN_COUNT_MASK | XIL_AXI_TIMER_CSR_ENABLE_INT_MASK | XIL_AXI_TIMER_CSR_AUTO_RELOAD_MASK | XIL_AXI_TIMER_CSR_CASC_MASK,
			tp->base_addr + XIL_AXI_TIMER_TCSR_OFFSET);

	// Enable interrupts1, and autoreload1, rest should be zero
	iowrite32(XIL_AXI_TIMER_CSR_DOWN_COUNT_MASK | XIL_AXI_TIMER_CSR_ENABLE_INT_MASK | XIL_AXI_TIMER_CSR_AUTO_RELOAD_MASK,
			tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);

    	// Disable timer0/counter
	// read data/previous config and then mask it with ~0b10000000
    	data = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR_OFFSET);
	iowrite32(data & ~(XIL_AXI_TIMER_CSR_ENABLE_TMR_MASK),
			tp->base_addr + XIL_AXI_TIMER_TCSR_OFFSET);
	
	// Disable timer1/counter
	// read data/previous config and then mask it with ~0b10000000	
	data = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);
	iowrite32(data & ~(XIL_AXI_TIMER_CSR_ENABLE_TMR_MASK),
			tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);
}


//***************************************************
// PROBE AND REMOVE
static int timer_probe(struct platform_device *pdev)
{ struct resource *r_mem;
	int rc = 0;

	// Get phisical register adress space from device tree
	r_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!r_mem) {
		printk(KERN_ALERT "xilaxitimer_probe: Failed to get reg resource\n");
		return -ENODEV;
	}

	// Get memory for structure timer_info
	tp = (struct timer_info *) kmalloc(sizeof(struct timer_info), GFP_KERNEL);
	if (!tp) {
		printk(KERN_ALERT "xilaxitimer_probe: Could not allocate timer device\n");
		return -ENOMEM;
	}

	// Put phisical adresses in timer_info structure
	tp->mem_start = r_mem->start;
	tp->mem_end = r_mem->end;

	// Reserve that memory space for this driver
	if (!request_mem_region(tp->mem_start,tp->mem_end - tp->mem_start + 1,	DEVICE_NAME))
	{
		printk(KERN_ALERT "xilaxitimer_probe: Could not lock memory region at %p\n",(void *)tp->mem_start);
		rc = -EBUSY;
		goto error1;
	}

	// Remap phisical to virtual adresses
	tp->base_addr = ioremap(tp->mem_start, tp->mem_end - tp->mem_start + 1);
	if (!tp->base_addr) {
		printk(KERN_ALERT "xilaxitimer_probe: Could not allocate memory\n");
		rc = -EIO;
		goto error2;
	}

	// Get interrupt number from device tree
	tp->irq_num = platform_get_irq(pdev, 0);
	if (!tp->irq_num) {
		printk(KERN_ALERT "xilaxitimer_probe: Failed to get irq resource\n");
		rc = -ENODEV;
		goto error2;
	}

	// Reserve interrupt number for this driver
	if (request_irq(tp->irq_num, xilaxitimer_isr, 0, DEVICE_NAME, NULL)) {
		printk(KERN_ERR "xilaxitimer_probe: Cannot register IRQ %d\n", tp->irq_num);
		rc = -EIO;
		goto error3;
	
	}
	else {
		printk(KERN_INFO "xilaxitimer_probe: Registered IRQ %d\n", tp->irq_num);
	}

	printk(KERN_NOTICE "xilaxitimer_probe: Timer platform driver registered\n");
	return 0;//ALL OK

error3:
	iounmap(tp->base_addr);
error2:
	release_mem_region(tp->mem_start, tp->mem_end - tp->mem_start + 1);
	kfree(tp);
error1:
	return rc;
}

static int timer_remove(struct platform_device *pdev)
{
	// Disable timer
    // TODO: change it for both TCSR 0 and 1
	unsigned int data=0;
	data = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR_OFFSET);
	iowrite32(data & ~(XIL_AXI_TIMER_CSR_ENABLE_TMR_MASK),
			tp->base_addr + XIL_AXI_TIMER_TCSR_OFFSET);

	data = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);
	iowrite32(data & ~(XIL_AXI_TIMER_CSR_ENABLE_TMR_MASK),
			tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);

	// Free resources taken in probe
	free_irq(tp->irq_num, NULL);
	iowrite32(0, tp->base_addr);
	iounmap(tp->base_addr);
	release_mem_region(tp->mem_start, tp->mem_end - tp->mem_start + 1);
	kfree(tp);
	printk(KERN_WARNING "xilaxitimer_remove: Timer driver removed\n");
	return 0;
}


//***************************************************
// FILE OPERATION functions

int timer_open(struct inode *pinode, struct file *pfile) 
{
	printk(KERN_INFO "Succesfully opened timer\n");
	return 0;
}

int timer_close(struct inode *pinode, struct file *pfile) 
{
	printk(KERN_INFO "Succesfully closed timer\n");
	//TODO: bring back after debugging
	//en_init = 1;
	return 0;
}

ssize_t timer_read(struct file *pfile, char __user *buffer, size_t length, loff_t *offset) 
{
	u32 cnt0 = 0;
	u64 cnt1 = 0;
	u64 cnt = 0;
   	
	int ret;
	int len = 0;
	int i = 0;
	char buff[BUFF_SIZE];
	
	if(endRead) {
		endRead = 0;
		return 0;
	}
	cnt0 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCR_OFFSET);
	cnt1 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCR1_OFFSET);
	cnt = (cnt1 << 32) | cnt0;

	//cnt = initial_value*100000 - cnt;
	//TODO: use length?
	printk(KERN_INFO "timer_read: cnt0 = %u\n", cnt0);
	printk(KERN_INFO "timer_read: cnt1 = %llu\n", cnt1);
	printk(KERN_INFO "timer_read: cnt = %llu\n", cnt);
	
	cnt = ((unsigned long long int)300000*100000 - cnt) + (initial_value - 300000)*100000;
	printk(KERN_INFO "timer_read after converting: cnt = %llu\n", cnt);
		
	for(i = 0; i < 8; ++i) // alernative: put every bit in buff. BUFF_SIZE would have to be at least 64!   
	{ 
		buff[i] = cnt >> (i*8);
		printk(KERN_INFO "timer_read: buff[%d] = %d\n", i, buff[i]);
	}
	len = 9;
	buff[8] = 0;

	ret = copy_to_user(buffer, buff, len);
	if(ret) 
		return -EFAULT;
	endRead = 1;
	printk(KERN_INFO "Succesfully read timer\n");
	return len;
}

ssize_t timer_write(struct file *pfile, const char __user *buffer, size_t length, loff_t *offset) 
{
	char buff[BUFF_SIZE];
	long long unsigned int millis = 0;
	int ret = 0;
	ret = copy_from_user(buff, buffer, length); // put into buff what is inside of buffer 
	if(ret)
		return -EFAULT;
	buff[length] = '\0';

	ret = sscanf(buff,"%c,%llu,%d",&mode,&millis,&en_init); // p,1000
	//TODO: change if needed
	if(en_init) {
		initial_value = millis + 300000; // First make initial value millis + five_min
		printk(KERN_INFO "xilaxitimer_write: initial setup entered:  %llu miliseconds \n", initial_value);
	}
	printk(KERN_INFO "xilaxitimer_write:  initial value: %llu miliseconds \n", initial_value);
	//millis = initial_value - millis; 
	if(ret == 3)//two parameters parsed in sscanf
	{   
		if (millis > initial_value)
		{
			printk(KERN_WARNING "xilaxitimer_write: Maximum period exceeded, enter something less than initial_value \n");
		}
		else
		{
			printk(KERN_INFO "xilaxitimer_write: millis %llu miliseconds \n", millis);
			setup_timer(millis);
		}		
		if(mode == 'p' || mode == 'P') {
		    printk(KERN_INFO "xilaxitimer_write: Pausing timer...");
		    stop_timer();
		}
		else
		if(mode == 's' || mode == 'S') {
		    printk(KERN_INFO "xilaxitimer_write: Starting timer...");
		    start_timer();
		}
		
	}

	else
	{
		printk(KERN_WARNING "xilaxitimer_write: Wrong format, expected n,t,k \n\t n-mode of operation[S/P]\n\t t-time in ms\n\t k- enable initial_value\n");
	}
	return length;
}

//***************************************************
// MODULE_INIT & MODULE_EXIT functions

static int __init timer_init(void)
{
	int ret = 0;


	ret = alloc_chrdev_region(&my_dev_id, 0, 1, DRIVER_NAME);
	if (ret){
		printk(KERN_ERR "xilaxitimer_init: Failed to register char device\n");
		return ret;
	}
	printk(KERN_INFO "xilaxitimer_init: Char device region allocated\n");

	my_class = class_create(THIS_MODULE, "timer_class");
	if (my_class == NULL){
		printk(KERN_ERR "xilaxitimer_init: Failed to create class\n");
		goto fail_0;
	}
	printk(KERN_INFO "xilaxitimer_init: Class created\n");

	my_device = device_create(my_class, NULL, my_dev_id, NULL, DRIVER_NAME);
	if (my_device == NULL){
		printk(KERN_ERR "xilaxitimer_init: Failed to create device\n");
		goto fail_1;
	}
	printk(KERN_INFO "xilaxitimer_init: Device created\n");

	my_cdev = cdev_alloc();	
	my_cdev->ops = &my_fops;
	my_cdev->owner = THIS_MODULE;
	ret = cdev_add(my_cdev, my_dev_id, 1);
	if (ret)
	{
		printk(KERN_ERR "xilaxitimer_init: Failed to add cdev\n");
		goto fail_2;
	}
	printk(KERN_INFO "xilaxitimer_init: Cdev added\n");
	printk(KERN_NOTICE "xilaxitimer_init: Hello world\n");

	return platform_driver_register(&timer_driver);

fail_2:
	device_destroy(my_class, my_dev_id);
fail_1:
	class_destroy(my_class);
fail_0:
	unregister_chrdev_region(my_dev_id, 1);
	return -1;
}

static void __exit timer_exit(void)
{
	platform_driver_unregister(&timer_driver);
	cdev_del(my_cdev);
	device_destroy(my_class, my_dev_id);
	class_destroy(my_class);
	unregister_chrdev_region(my_dev_id,1);
	printk(KERN_INFO "xilaxitimer_exit: Goodbye, cruel world\n");
}


module_init(timer_init);
module_exit(timer_exit);
