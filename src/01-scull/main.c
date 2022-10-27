/*
 * main.c -- the bare scull char module
 *
 * Copyright (C) 2001 Alessandro Rubini and Jonathan Corbet
 * Copyright (C) 2001 O'Reilly & Associates
 *
 * The source code in this file can be freely used, adapted,
 * and redistributed in source or binary form, so long as an
 * acknowledgment appears in derived source files.  The citation
 * should list that the code comes from the book "Linux Device
 * Drivers" by Alessandro Rubini and Jonathan Corbet, published
 * by O'Reilly & Associates.   No warranty is attached;
 * we cannot take responsibility for errors or fitness for use.
 *
 */

/*
 * Author: zgy
 * Time: 2022-10-04
 *
 * Add comments to understand the book contents
 */

// #include <linux/config.h> /* zgy: deprecated header */
#include <linux/module.h> /* zgy: module.h, init.h, two must have headers */
#include <linux/init.h>
#include <linux/moduleparam.h> /* zgy: module parameters export permission flags here */

#include <linux/cdev.h>
#include <linux/errno.h>  /* error codes */
#include <linux/fcntl.h>  /* O_ACCMODE */
#include <linux/fs.h>     /* everything... */
#include <linux/kernel.h> /* printk() */
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>  /* kmalloc() */
#include <linux/types.h> /* size_t */

#include <linux/version.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(3,3,0)
  #include <asm/switch_to.h>
#else
  #include <asm/system.h>  /* cli(), *_flags */
#endif
#include <asm/uaccess.h> /* copy_*_user */

#include "scull.h" /* local definitions */

/*
 * Our parameters which can be set at load time.
 */

// zgy: exporeted module parameters
int scull_major = SCULL_MAJOR;
int scull_minor = 0;
int scull_nr_devs = SCULL_NR_DEVS; /* number of bare scull devices */
int scull_quantum = SCULL_QUANTUM;
int scull_qset = SCULL_QSET;

// zgy: all of them can only be read from sysfs
module_param(scull_major, int, S_IRUGO);
module_param(scull_minor, int, S_IRUGO);
module_param(scull_nr_devs, int, S_IRUGO);
module_param(scull_quantum, int, S_IRUGO);
module_param(scull_qset, int, S_IRUGO);

MODULE_AUTHOR("Alessandro Rubini, Jonathan Corbet");
MODULE_LICENSE("Dual BSD/GPL");

struct scull_dev *scull_devices; /* allocated in scull_init_module */

/*
 * Empty out the scull device; must be called with the device
 * semaphore held.
 */
int scull_trim(struct scull_dev *dev) {
  struct scull_qset *next, *dptr;
  int qset = dev->qset; /* "dev" is not-null */
  int i;

  // zgy: Each devices has multiple qset, each qset contains an array of quantum.
  // free the memory used by them.
  for (dptr = dev->data; dptr; dptr = next) { /* all the list items */
    if (dptr->data) {
      for (i = 0; i < qset; i++)
        kfree(dptr->data[i]);
      kfree(dptr->data);
      dptr->data = NULL;
    }
    next = dptr->next;
    kfree(dptr);
  }
  // zgy: initialization
  dev->size = 0;
  dev->quantum = scull_quantum;
  dev->qset = scull_qset;
  dev->data = NULL;
  return 0;
}

/*
 * Open and close
 */

int scull_open(struct inode *inode, struct file *filp) {
  // zgy: filp will be return to the user. It is used as the target for following
  // operations.

  struct scull_dev *dev; /* device information */

  // zgy: get the char dev struct through inode
  dev = container_of(inode->i_cdev, struct scull_dev, cdev);
  filp->private_data = dev; /* for other methods */

  /* now trim to 0 the length of the device if open was write-only */
  if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
    if (down_interruptible(&dev->sem))
    // if (mutex_lock_interruptible(&dev->sem))
      return -ERESTARTSYS;
    scull_trim(dev); /* ignore errors */
    // mutex_unlock(&dev->sem);
    up(&dev->sem);
  }
  return 0; /* success */
}

int scull_release(struct inode *inode, struct file *filp) { return 0; }
/*
 * Follow the list
 */
struct scull_qset *scull_follow(struct scull_dev *dev, int n) {
  struct scull_qset *qs = dev->data;

  /* Allocate first qset explicitly if need be */
  if (!qs) {
    qs = dev->data = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
    if (qs == NULL)
      return NULL; /* Never mind */
    memset(qs, 0, sizeof(struct scull_qset));
  }

  /* Then follow the list */
  while (n--) {
    if (!qs->next) {
      qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
      if (qs->next == NULL)
        return NULL; /* Never mind */
      memset(qs->next, 0, sizeof(struct scull_qset));
    }
    qs = qs->next;
    continue;
  }
  return qs;
}

/*
 * Data management: read and write
 */

ssize_t scull_read(struct file *filp, char __user *buf, size_t count,
                   loff_t *f_pos) {
  // zgy: without open(), we cannot get dev
  struct scull_dev *dev = filp->private_data;
  struct scull_qset *dptr; /* the first listitem */
  int quantum = dev->quantum, qset = dev->qset;
  int itemsize = quantum * qset; /* how many bytes in the listitem */
  int item, s_pos, q_pos, rest;
  ssize_t retval = 0;

  if (down_interruptible(&dev->sem))
  // if (mutex_lock_interruptible(&dev->sem))
    return -ERESTARTSYS;
  if (*f_pos >= dev->size)
    goto out;
  if (*f_pos + count > dev->size)
    count = dev->size - *f_pos;

  /* find listitem, qset index, and offset in the quantum */
  // zgy: itemsize is the size of a qset (by default = 4000 * 1000)
  item = (long)*f_pos / itemsize;
  rest = (long)*f_pos % itemsize;
  s_pos = rest / quantum;
  q_pos = rest % quantum;

  /* follow the list up to the right position (defined elsewhere) */
  dptr = scull_follow(dev, item);

  if (dptr == NULL || !dptr->data || !dptr->data[s_pos])
    goto out; /* don't fill holes */

  /* read only up to the end of this quantum */
  if (count > quantum - q_pos)
    count = quantum - q_pos;

  if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
    retval = -EFAULT;
    goto out;
  }
  *f_pos += count;
  retval = count;

out:
  // mutex_unlock(&dev->sem);
  up(&dev->sem);
  return retval;
}

ssize_t scull_write(struct file *filp, const char __user *buf, size_t count,
                    loff_t *f_pos) {
  struct scull_dev *dev = filp->private_data;
  struct scull_qset *dptr;
  int quantum = dev->quantum, qset = dev->qset;
  int itemsize = quantum * qset;
  int item, s_pos, q_pos, rest;
  // zgy: a good practice, false first
  ssize_t retval = -ENOMEM; /* value used in "goto out" statements */

  // if (mutex_lock_interruptible(&dev->sem))
  if (down_interruptible(&dev->sem))
    return -ERESTARTSYS;

  /* find listitem, qset index and offset in the quantum */
  item = (long)*f_pos / itemsize;
  rest = (long)*f_pos % itemsize;
  s_pos = rest / quantum;
  q_pos = rest % quantum;

  /* follow the list up to the right position */
  dptr = scull_follow(dev, item);
  if (dptr == NULL)
    goto out;
  // zgy: qset is just an array of pointers
  if (!dptr->data) {
    dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL);
    if (!dptr->data)
      goto out;
    memset(dptr->data, 0, qset * sizeof(char *));
  }
  // pointer to the quantum
  if (!dptr->data[s_pos]) {
    dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);
    if (!dptr->data[s_pos])
      goto out;
  }
  /* write only up to the end of this quantum */
  if (count > quantum - q_pos)
    count = quantum - q_pos;

  if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)) {
    retval = -EFAULT;
    goto out;
  }
  *f_pos += count;
  retval = count;

  /* update the size */
  if (dev->size < *f_pos)
    dev->size = *f_pos;

out:
  // mutex_unlock(&dev->sem);
  up(&dev->sem);
  return retval;
}

/*
 * The "extended" operations -- only seek
 */

loff_t scull_llseek(struct file *filp, loff_t off, int whence) {
  struct scull_dev *dev = filp->private_data;
  loff_t newpos;

  switch (whence) {
  case 0: /* SEEK_SET */
    newpos = off;
    break;

  case 1: /* SEEK_CUR */
    newpos = filp->f_pos + off;
    break;

  case 2: /* SEEK_END */
    newpos = dev->size + off;
    break;

  default: /* can't happen */
    return -EINVAL;
  }
  if (newpos < 0)
    return -EINVAL;
  filp->f_pos = newpos;
  return newpos;
}

struct file_operations scull_fops = {
    .owner = THIS_MODULE,
    .llseek = scull_llseek,
    .read = scull_read,
    .write = scull_write,
    .unlocked_ioctl = NULL,
    .open = scull_open,
    .release = scull_release,
};

/*
 * Finally, the module stuff
 */

/*
 * The cleanup function is used to handle initialization failures as well.
 * Thefore, it must be careful to work correctly even if some of the items
 * have not been initialized
 */
void scull_cleanup_module(void) {
  int i;
  dev_t devno = MKDEV(scull_major, scull_minor);

  /* Get rid of our char dev entries */
  if (scull_devices) {
    for (i = 0; i < scull_nr_devs; i++) {
      scull_trim(scull_devices + i);
      cdev_del(&scull_devices[i].cdev);
    }
    kfree(scull_devices);
  }

  /* cleanup_module is never called if registering failed */
  unregister_chrdev_region(devno, scull_nr_devs);

  /* and call the cleanup functions for friend devices */
  // scull_p_cleanup();
  // scull_access_cleanup();
  
  printk("scull: Cleaned up, bye bye!\n");
}

/*
 * Set up the char_dev structure for this device.
 */
static void scull_setup_cdev(struct scull_dev *dev, int index) {
  int err, devno = MKDEV(scull_major, scull_minor + index);

  cdev_init(&dev->cdev, &scull_fops); // zgy: init the char dev management struct
  dev->cdev.owner = THIS_MODULE;
  dev->cdev.ops = &scull_fops;
  err = cdev_add(&dev->cdev, devno, 1); // zgy: add this dev to kernel.
  /* Fail gracefully if need be */
  if (err)
    printk(KERN_NOTICE "Error %d adding scull%d", err, index);
}

// zgy: the module init funtion
int scull_init_module(void) {
  printk("scull: Hello, we will start to initialize the driver\n");

  int result, i;
  dev_t dev = 0;

  /*
   * Get a range of minor numbers to work with, asking for a dynamic
   * major unless directed otherwise at load time.
   */

  /* zgy: tells the kernel a certain region is needed by this driver.
   * by default, we dynamically allocate major number from kernel.
   * Not zero means the user gives a specific nr during insmod
   */
  if (scull_major) {
    dev = MKDEV(scull_major, scull_minor);
    result = register_chrdev_region(dev, scull_nr_devs, "scull");
  } else {
    // zgy: dymanically allcoate major number for this driver
    result = alloc_chrdev_region(&dev, scull_minor, scull_nr_devs, "scull");
    scull_major = MAJOR(dev);
  }
  // zgy: allocation can fail
  if (result < 0) {
    printk(KERN_WARNING "scull: can't get major %d\n", scull_major);
    return result;
  }

  /*
   * allocate the devices -- we can't have them static, as the number
   * can be specified at load time
   */
  // zyg: fake devices simulated by memory
  scull_devices = kmalloc(scull_nr_devs * sizeof(struct scull_dev), GFP_KERNEL);
  if (!scull_devices) {
    result = -ENOMEM;
    goto fail; /* Make this more graceful */
  }
  memset(scull_devices, 0, scull_nr_devs * sizeof(struct scull_dev));

  /* Initialize each device. */
  for (i = 0; i < scull_nr_devs; i++) {
    scull_devices[i].quantum = scull_quantum;
    scull_devices[i].qset = scull_qset;
    //mutex_init(&scull_devices[i].sem);
    sema_init(&scull_devices[i].sem, 1);
    scull_setup_cdev(&scull_devices[i], i);
  }

  /* At this point call the init function for any friend device */
  /* dev = MKDEV(scull_major, scull_minor + scull_nr_devs);
  dev += scull_p_init(dev);
  dev += scull_access_init(dev); */

  return 0; /* succeed */

fail:
  scull_cleanup_module();
  return result;
}

module_init(scull_init_module);
module_exit(scull_cleanup_module);
