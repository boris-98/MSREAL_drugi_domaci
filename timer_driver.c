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

#include <linux/math64.h>

// REGISTER CONSTANTS
#define XIL_AXI_TIMER_TCSR0_OFFSET	0x0		//timer0
#define XIL_AXI_TIMER_TLR0_OFFSET		0x4
#define XIL_AXI_TIMER_TCR0_OFFSET		0x8
#define XIL_AXI_TIMER_TCSR1_OFFSET	0x10		//timer1
#define XIL_AXI_TIMER_TLR1_OFFSET		0x14
#define XIL_AXI_TIMER_TCR1_OFFSET		0x18

#define XIL_AXI_TIMER_CSR_CASC_MASK	0x00000800
#define XIL_AXI_TIMER_CSR_ENABLE_ALL_MASK	0x00000400
#define XIL_AXI_TIMER_CSR_ENABLE_PWM_MASK	0x00000200
#define XIL_AXI_TIMER_CSR_INT_OCCURED_MASK 0x00000100
#define XIL_AXI_TIMER_CSR_ENABLE_TMR_MASK 0x00000080
#define XIL_AXI_TIMER_CSR_ENABLE_INT_MASK 0x00000040
#define XIL_AXI_TIMER_CSR_LOAD_MASK 0x00000020
#define XIL_AXI_TIMER_CSR_AUTO_RELOAD_MASK 0x00000010
#define XIL_AXI_TIMER_CSR_EXT_CAPTURE_MASK 0x00000008
#define XIL_AXI_TIMER_CSR_EXT_GENERATE_MASK 0x00000004
#define XIL_AXI_TIMER_CSR_DOWN_COUNT_MASK 0x00000002
#define XIL_AXI_TIMER_CSR_CAPTURE_MODE_MASK 0x00000001

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


unsigned char startFlag = 0;


static void start_timer(void);
static void stop_timer(void);
static irqreturn_t xilaxitimer_isr(int irq,void*dev_id);
static void setup_timer(u64 milliseconds);
static int timer_probe(struct platform_device *pdev);
static int timer_remove(struct platform_device *pdev);
int timer_open(struct inode *pinode, struct file *pfile);
int timer_close(struct inode *pinode, struct file *pfile);
ssize_t timer_read(struct file *pfile, char __user *buffer, size_t length, loff_t *offset);
ssize_t timer_write(struct file *pfile, const char __user *buffer, size_t length, loff_t *offset);
static int __init timer_init(void);
static void __exit timer_exit(void);

struct file_operations my_fops =
{
	.owner = THIS_MODULE,
	.open = timer_open,
	.read = timer_read,
	.write = timer_write,
	.release = timer_close,
};

static struct of_device_id timer_of_match[] = {
	{ .compatible = "xlnx,xps-timer-1.00.a", },
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
	unsigned int data0 = 0;

	// Check Timer Counter Value
	data0 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCR0_OFFSET);

	printk(KERN_INFO "xilaxitimer_isr: Interrupt has occurred !\n");

	// Clear Interrupt
	data0 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
	iowrite32(data0 | XIL_AXI_TIMER_CSR_INT_OCCURED_MASK,
			tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);

	// Disable Timer
		printk(KERN_NOTICE "xilaxitimer_isr: Time's up!\n");
		data0 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
		iowrite32(data0 & ~(XIL_AXI_TIMER_CSR_ENABLE_ALL_MASK), tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
		startFlag = 0;		

	return IRQ_HANDLED;
}
//***************************************************
//HELPER FUNCTION THAT SETS UP TIMER

static void setup_timer(u64 milliseconds)
{
	// Disable Timer Counter
	unsigned int timer0_load;
	unsigned int timer1_load;
	u64 timer_load;
	unsigned int data0 = 0;
	unsigned int data1 = 0;
	timer_load = milliseconds*100000;
	timer0_load = (unsigned int)timer_load;
	timer_load >>= 32;
	timer1_load = (unsigned int)timer_load;

	// Disable timer/counter while configuration is in progress
	data0 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
	iowrite32(data0 & ~(XIL_AXI_TIMER_CSR_ENABLE_TMR_MASK),
			tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
	data1 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);
	iowrite32(data1 & ~(XIL_AXI_TIMER_CSR_ENABLE_TMR_MASK),
			tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);
	
	// Set initial value in load registers
	iowrite32(timer0_load, tp->base_addr + XIL_AXI_TIMER_TLR0_OFFSET);
	iowrite32(timer1_load, tp->base_addr + XIL_AXI_TIMER_TLR1_OFFSET);

	// Load initial value into counter from load registers

	data0 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
	iowrite32(data0 | XIL_AXI_TIMER_CSR_LOAD_MASK,
			tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
	data1 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);	
	iowrite32(data1 | XIL_AXI_TIMER_CSR_LOAD_MASK,
			tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);


	data0 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
	iowrite32(data0 & ~(XIL_AXI_TIMER_CSR_LOAD_MASK),
			tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
	data1 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);
	iowrite32(data1 & ~(XIL_AXI_TIMER_CSR_LOAD_MASK),
			tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);

	// Enable interrupts, cascade and downcount, rest should be zero
	iowrite32(XIL_AXI_TIMER_CSR_ENABLE_INT_MASK | XIL_AXI_TIMER_CSR_CASC_MASK 
						    | XIL_AXI_TIMER_CSR_DOWN_COUNT_MASK,
			tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);

	printk(KERN_INFO "Timer is set up!\n");		

}

static void start_timer()
{
	unsigned int data0 = 0;
	data0 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
	iowrite32(data0 | XIL_AXI_TIMER_CSR_ENABLE_ALL_MASK,
				tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
	startFlag = 1;
	printk(KERN_INFO "Timer has started!\n");	
}

static void stop_timer()
{
	unsigned int data0 = 0;
	unsigned int data1 = 0;
	unsigned int timer0_load = 0;
	unsigned int timer1_load = 0;
	
	timer0_load = ioread32(tp->base_addr + XIL_AXI_TIMER_TCR0_OFFSET);
	timer1_load = ioread32(tp->base_addr + XIL_AXI_TIMER_TCR1_OFFSET);
	
	//disabling
	data0 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
	iowrite32(data0 & ~(XIL_AXI_TIMER_CSR_ENABLE_TMR_MASK),
			tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
	data1 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);
	iowrite32(data1 & ~(XIL_AXI_TIMER_CSR_ENABLE_TMR_MASK),
			tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);

	//set initial values
	iowrite32(timer1_load, tp->base_addr + XIL_AXI_TIMER_TLR1_OFFSET);
	iowrite32(timer0_load, tp->base_addr + XIL_AXI_TIMER_TLR0_OFFSET);

	//load initial values
	data0 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
	iowrite32(data0 | XIL_AXI_TIMER_CSR_LOAD_MASK,
				tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);

	data1 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);
	iowrite32(data1 | XIL_AXI_TIMER_CSR_LOAD_MASK,
				tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);
 
	data0 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
	iowrite32(data0 & ~(XIL_AXI_TIMER_CSR_LOAD_MASK),
			tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
	data1 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);
	iowrite32(data1 & ~(XIL_AXI_TIMER_CSR_LOAD_MASK),
			tp->base_addr + XIL_AXI_TIMER_TCSR1_OFFSET);

	//setting up	
	iowrite32(XIL_AXI_TIMER_CSR_ENABLE_INT_MASK | XIL_AXI_TIMER_CSR_CASC_MASK
						    | XIL_AXI_TIMER_CSR_DOWN_COUNT_MASK,
						tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
	
	startFlag = 0;
	printk(KERN_INFO "Time has stopped!\n");
}

//***************************************************
// PROBE AND REMOVE
static int timer_probe(struct platform_device *pdev)
{
	struct resource *r_mem;
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
	unsigned int data=0;
	data = ioread32(tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
	iowrite32(data & ~(XIL_AXI_TIMER_CSR_ENABLE_ALL_MASK),
			tp->base_addr + XIL_AXI_TIMER_TCSR0_OFFSET);
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
	//printk(KERN_INFO "Succesfully opened timer\n");
	return 0;
}

int timer_close(struct inode *pinode, struct file *pfile) 
{
	//printk(KERN_INFO "Succesfully closed timer\n");
	return 0;
}

ssize_t timer_read(struct file *pfile, char __user *buffer, size_t length, loff_t *offset) 
{
	u64 time = 0;
	u64 time_seconds = 0;
	u64 temp1 = 0;
	u64 temp2 = 0;
	u64 days = 0;
	u64 hours = 0;
	u64 minutes = 0;
	u64 seconds = 0;	
	unsigned int data0 = 0;
	unsigned int data1 = 0;

	int ret;
	char buff[BUFF_SIZE];
	long int len = 0;	

	data1 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCR1_OFFSET);
	data0 = ioread32(tp->base_addr + XIL_AXI_TIMER_TCR0_OFFSET);

	time += data1;
	time = (time << 32) + data0;
	time_seconds = div64_u64(time, 100000*1000);	

	days = div64_u64_rem(time_seconds, 86400, &temp1);
	hours = div64_u64_rem(temp1, 3600, &temp2);
	minutes = div64_u64_rem(temp2, 60, &seconds);

	len = scnprintf(buff, BUFF_SIZE, "%llu %llu %llu %llu %llu\n", time_seconds, days, hours, minutes, seconds);
	ret = copy_to_user(buffer, buff, len);
	
	printk(KERN_INFO "Remaining time: %llu:%llu:%llu:%llu\n", days, hours, minutes, seconds);

	if(ret)
		return -EFAULT;
		
	return 0;
}

ssize_t timer_write(struct file *pfile, const char __user *buffer, size_t length, loff_t *offset) 
{
	char buff[BUFF_SIZE];
	char command[BUFF_SIZE];
	char start[] = "start";
	char stop[] = "stop";
	unsigned int days = 0;
	unsigned int hours = 0;
	unsigned int minutes = 0;
	unsigned int seconds = 0;
	u64  millis = 0;
	int i = 0;	

	int ret = 0;
	ret = copy_from_user(buff, buffer, length);
	if(ret)
		return -EFAULT;
	buff[length] = '\0';

	ret = sscanf(buff,"%d:%d:%d:%d",&days, &hours, &minutes, &seconds);
	if(ret == 4)//four parameters parsed in sscanf
	{

		if (days > 2135014) //2135039 je max dana za 64bitni timer
		{
			printk(KERN_WARNING "xilaxitimer_write: Maximum period exceeded, enter something less than that \n");
		}
		else if(days==0 && hours==0 && minutes ==0 && seconds ==0)
		{
			printk(KERN_WARNING "xilaxtimer_write: Enter something greater than 0\n");
		}
		else
		{
			millis = 0;
			millis = seconds*1000 + minutes*60*1000 + hours*3600*1000 + days*86400*1000;

			printk(KERN_INFO "xilaxitimer_write: Setting up timer for %d days, %d hours, %d minutes, %d seconds",days, hours, minutes, seconds);
			setup_timer(millis);
		}

	}
	else
	{
		for(i = 0; i < length; i++)
		{
			command[i] = buff[i];
		}		
		command[length] = '\0';	

		if(!strncmp(command, start, strlen(start)))  //iz nekog razloga nece strcmp
		{
			if(startFlag == 0)
			{
				printk(KERN_INFO "Starting timer\n");
				start_timer();
			}
			else
			{
				printk(KERN_INFO "Timer is already started!\n");
			}
		}
		else if(!strncmp(command, stop, strlen(stop)))
		{
			if(startFlag == 1)
			{
				printk(KERN_INFO "Stopping timmer\n");
				stop_timer();
			}
			else
			{
				printk(KERN_INFO "Timer is already stopped!\n");
			}
		}
		else
		{
			printk(KERN_WARNING "xilaxitimer_write: Wrong format!\n");
		}
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
