//////////////////////////////////////////////////////////////////////
//                      North Carolina State University
//
//
//
//                             Copyright 2018
//
////////////////////////////////////////////////////////////////////////
//
// This program is free software; you can redistribute it and/or modify it
// under the terms and conditions of the GNU General Public License,
// version 2, as published by the Free Software Foundation.
//
// This program is distributed in the hope it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
//
////////////////////////////////////////////////////////////////////////
//
//   Author:  Hung-Wei Tseng, Yu-Chia Liu
//
//   Description:
//     Core of Kernel Module for Processor Container
//
////////////////////////////////////////////////////////////////////////

#include "memory_container.h"

#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/list.h>

//extern struct p_container p_cont;
//extern struct p_container* find_container(unsigned long cid);
//extern  struct p_container* add_container(unsigned long cid);
//extern struct list_head container_list;

static LIST_HEAD(container_list);
static LIST_HEAD(object_list);
static DEFINE_MUTEX(container_lock);
static DEFINE_MUTEX(object_lock);

/*
 * Structure for tasks within a container
 */
struct p_cont_task
{
    int tid;
    struct list_head list;
};

/**
 * Structure for a container
 */
struct p_container
{
    unsigned long cid;
    struct p_cont_task* task_head;
    /* Task counter */
    int task_counter;
    int obj_counter;
    struct list_head c_list;
    /* List of objects */
    //struct list_head object_list;
};

/**
 * Structure for Object linked list
 */
struct object
{
    unsigned long long  oid;
    unsigned long cid;
    char* data;
    struct list_head o_list;
};


struct object *find_object(unsigned long cid, unsigned long oid)
{
    struct object* obj;
    /* Traverse/loop through the linked list */
    list_for_each_entry(obj, &(object_list), o_list)
    {
            if (obj->cid == cid && obj->oid == oid)
            {
                return obj;
            }
    }
    return NULL;
}

struct p_container *find_container(unsigned long cid)
{
    struct p_container *c;
    /* Traverse/loop through the linked list */
    list_for_each_entry(c, &container_list, c_list)
        {
            if (c->cid==cid)
                {
                    return c;
                }
        }
    return NULL;
}


/*
 * display() prints out the container_list
 */
void display(void) {
    struct p_container* tmp_cont;
    struct list_head* pos;

    printk(KERN_ERR "Printing container list\n");
    list_for_each(pos, &container_list) {
        tmp_cont = list_entry(pos, struct p_container, c_list);
        printk(KERN_INFO "Container id: %lu\n", tmp_cont->cid);
    }
}

/**
 * find the container to which the current task belongs
 */
struct p_container* find_container_by_task(void)
{
    struct p_container *c;
    struct p_cont_task *t;
    list_for_each_entry(c, &container_list,c_list)
    {
        struct p_cont_task *task_head = c->task_head;
        list_for_each_entry(t, &task_head->list, list)
        {
            if(t->tid == current->pid)
                return c;
        }
    }
    return NULL;
}

/*
 * Deletes the container whose task_counter is 0
 */
bool delete_container_if_empty(struct p_container *curr_container)
{
    struct list_head *pos, *q;
    struct p_container* tmp;

    // Iterate over the container_list to find the curr_container.
    // Delete it if all tasks and all objects in it have been deleted,
    // i.e. task_counter = 0 && obj_counter = 0
    list_for_each_safe(pos, q, &container_list)
    {
        tmp = list_entry(pos, struct p_container, c_list);

        //printk(KERN_INFO "Container task counter is %d", tmp->task_counter);
        if ((tmp->cid == curr_container->cid)
            && (tmp->task_counter == 0) && tmp->obj_counter == 0)
        {
            list_del(pos);
            printk(KERN_INFO "Deleted container with id: %lu", tmp->cid);
            kfree(tmp);
            //display();
            return true;
        }
    }
    return false;
}

int memory_container_delete(struct memory_container_cmd __user *user_cmd)
{
    int task_id = current->pid;
    struct p_container *curr_container;
    struct p_cont_task* task_head = NULL;
    struct p_cont_task *temp = NULL;
    struct list_head *pos, *t;

    //printk(KERN_ERR "We want to delete the task with task id %d \n", task_id);

    // Entering critical section.

    mutex_lock(&container_lock);
    curr_container = find_container_by_task();
    // This if condition should never get called
    if (curr_container == NULL)
    {
        printk(KERN_ERR "Container does not exist");
        return 1;
    }
    //mutex_lock(&container_lock);

    task_head = curr_container->task_head;

    // Iterate over task list to find the task that must be deleted.
    // Delete it and decrement the task_counter of the corresponding container.
    list_for_each_safe(pos, t, &(task_head->list))
    {
        temp = list_entry(pos, struct p_cont_task, list);
        if (temp->tid == task_id) {
            list_del(pos);
            printk("Deleting item with tid %d \n", temp->tid);
            curr_container->task_counter--;
            delete_container_if_empty(curr_container);
            kfree(temp);
            break;
        }
    }
    mutex_unlock(&container_lock);
    // Critical section ends.
    return 0;
}

/**
 * Add a task to the given container
 * Tasks are stored in kernel's built-in linked lists.
 * We have a pointer to the p_cont_task in the container,
 * which stores the circular doubly linked list of tasks
 */
void add_task(struct p_container *cont)
{
    struct p_cont_task *temp = NULL;
    struct p_cont_task *task_head = NULL;

    //TODO: Delete the debugging comments later

    // Create temp which will hold the current task's value that we wish to
    // associate with the container
    temp = (struct p_cont_task*)
	    kmalloc(sizeof(struct p_cont_task), GFP_KERNEL);
    temp->tid = current->pid;
    // Using INIT_LIST_HEAD will make temp point to itself thus creating a
    // circular doubly linked list with one element. This could have been
    // omitted but it's better to handle everything.
    INIT_LIST_HEAD(&(temp->list));

    // If the container does not have any task associated with it yet,
    // the task_list within it will be empty. We need to initialize it.
    if (cont->task_head == NULL)
    {
	    task_head = (struct p_cont_task*)
	    kmalloc(sizeof(struct p_cont_task), GFP_KERNEL);

	    // INIT_LIST_HEAD will make. This initialized a dynamically allocated
	    // linked list head. It'll make task_head point to itself thus creating
        // a circular doubly linked list with one element
        INIT_LIST_HEAD(&task_head->list);
        list_add(&(temp->list), &(task_head->list));
        cont->task_head = task_head;
    }
    else {
        list_add(&(temp->list), &(cont->task_head->list));
    }
    printk(KERN_INFO "Added task with TID : %d to container with CID: %lu\n",current->pid,cont->cid);
    cont->task_counter++;
}

struct p_container *add_container(unsigned long cid)
{
    struct p_container *c;

    c = kmalloc(sizeof(struct p_container), GFP_KERNEL);

    if (c == NULL)
        {
            printk(KERN_ERR "Error in allocating memory for container with CID %d",
                   cid);
            return NULL;
        }
    c->cid=cid;
    /* Initializing task_counter = 0 and task_head = NULL since this container
     ** has no tasks associated yet.
     */
    c->task_counter = 0;
    c->obj_counter = 0;
    c->task_head = NULL;
    INIT_LIST_HEAD(&(c->c_list));
    list_add(&(c->c_list), &container_list);

    printk(KERN_INFO "After adding container");
    display();
    return c;
}


/* Get container for current task,
** If object does not  exit.
**     Create object
** return object
*/

int memory_container_mmap(struct file *filp, struct vm_area_struct *vma)
{
    size_t length = 0;
    struct p_container* curr_cont = NULL;
    unsigned long cid = 0, pfn = 0;
    struct object* obj = NULL;

    printk(KERN_INFO "HAHAHA In memory_container_mmap\n");
    //printk(KERN_INFO "Start address is: %llu\n", vma->vm_start);
    //printk(KERN_INFO "End address is: %llu\n", vma->vm_end);
    length = vma->vm_end - vma->vm_start;

    //printk(KERN_INFO "Length is %d", length);
    printk(KERN_INFO "Offset*getpagesize is: %ld", vma->vm_pgoff);

    //printk(KERN_INFO "New and updated\n");

    mutex_lock(&container_lock);
    curr_cont = find_container_by_task();
    cid = curr_cont->cid;
    obj = find_object(cid, vma->vm_pgoff);
    if (!obj)
    {
        printk(KERN_INFO "Object does not exist \n");
        obj = kmalloc(sizeof(struct object), GFP_KERNEL);
        obj->data = kmalloc(length, GFP_KERNEL);
        obj->oid = vma->vm_pgoff;
        obj->cid = cid;
        list_add(&(obj->o_list), &(object_list));
        curr_cont->obj_counter++;
    }
    else
        printk(KERN_INFO "Object exists\n");

    pfn = virt_to_phys((void *) obj->data)>>PAGE_SHIFT;
    remap_pfn_range(vma, vma->vm_start, pfn, length, vma->vm_page_prot);
    mutex_unlock(&container_lock);

    return 0;
}


int memory_container_lock(struct memory_container_cmd __user *user_cmd)
{
    mutex_lock(&object_lock);
    return 0;
}


int memory_container_unlock(struct memory_container_cmd __user *user_cmd)
{
    mutex_unlock(&object_lock);
    return 0;
}


/*int memory_container_delete(struct memory_container_cmd __user *user_cmd)
{
    return 0;
    }*/


int memory_container_create(struct memory_container_cmd __user *user_cmd)
{
    struct memory_container_cmd cmd;
    struct p_container* curr_container = NULL;

    mutex_lock(&container_lock);
    copy_from_user(&cmd, user_cmd, sizeof(struct memory_container_cmd));
    printk(KERN_INFO "HAHA In create cid received %llu \n",cmd.cid);
    curr_container = find_container(cmd.cid);
    if (!curr_container)
    {
        curr_container = add_container(cmd.cid);
    }
    add_task(curr_container);
    mutex_unlock(&container_lock);
    return 0;
}


int memory_container_free(struct memory_container_cmd __user *user_cmd)
{
    // Container lock is not needed as there will never be a scenario where we
    // are trying to free and obj from an container & deleting a container at
    // the same time
    struct memory_container_cmd cmd;
    struct p_container* curr_container = NULL;
    struct object* temp = NULL;
    struct list_head *pos, *t;

    printk(KERN_INFO "In memory_container_free\n");
    copy_from_user(&cmd, user_cmd, sizeof(struct memory_container_cmd));
    curr_container = find_container_by_task();
    if(curr_container == NULL)
    {
        printk(KERN_ERR "Container does not exist\n");
        return 1;
    }
    list_for_each_safe(pos,t,&object_list)
    {
        temp = list_entry(pos, struct object, o_list);
        if(temp->cid == curr_container->cid && temp->oid == cmd.oid)
        {
            list_del(pos);
            printk("Deleting object with id %llu from container %lu \n ",
                   cmd.oid,  curr_container->cid);
            kfree(temp->data);
            kfree(temp);
            curr_container->obj_counter--;
            break;
        }
    }
    return 0;
}


/**
 * control function that receive the command in user space and pass arguments to
 * corresponding functions.
 */
int memory_container_ioctl(struct file *filp, unsigned int cmd,
                              unsigned long arg)
{
    switch (cmd)
    {
    case MCONTAINER_IOCTL_CREATE:
        return memory_container_create((void __user *)arg);
    case MCONTAINER_IOCTL_DELETE:
        return memory_container_delete((void __user *)arg);
    case MCONTAINER_IOCTL_LOCK:
        return memory_container_lock((void __user *)arg);
    case MCONTAINER_IOCTL_UNLOCK:
        return memory_container_unlock((void __user *)arg);
    case MCONTAINER_IOCTL_FREE:
        return memory_container_free((void __user *)arg);
    default:
        return -ENOTTY;
    }
}
