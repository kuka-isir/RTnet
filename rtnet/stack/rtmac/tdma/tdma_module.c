/***
 *
 *  rtmac/tdma/tdma_module.c
 *
 *  RTmac - real-time networking media access control subsystem
 *  Copyright (C) 2002       Marc Kleine-Budde <kleine-budde@gmx.de>,
 *                2003, 2004 Jan Kiszka <Jan.Kiszka@web.de>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <asm/div64.h>
#include <asm/semaphore.h>
#include <linux/init.h>
#include <linux/module.h>

#include <rtnet_sys.h>
#include <rtmac/tdma/tdma.h>
#include <rtmac/tdma/tdma_dev.h>
#include <rtmac/tdma/tdma_ioctl.h>
#include <rtmac/tdma/tdma_proto.h>
#include <rtmac/tdma/tdma_worker.h>


/* RTAI-specific: start scheduling timer */
#if defined(CONFIG_RTAI_24) || defined(CONFIG_RTAI_30) || \
    defined(CONFIG_RTAI_31) || defined(CONFIG_RTAI_32)
static int start_timer = 0;

MODULE_PARM(start_timer, "i");
MODULE_PARM_DESC(start_timer, "set to non-zero to start RTAI timer");
#endif

#ifdef CONFIG_PROC_FS
LIST_HEAD(tdma_devices);
DECLARE_MUTEX(tdma_nrt_lock);


int tdma_proc_read(char *buf, char **start, off_t offset, int count,
                    int *eof, void *data)
{
    struct tdma_priv    *entry;
#ifdef CONFIG_RTNET_TDMA_MASTER
    nanosecs_t          cycle;
#endif
    RTNET_PROC_PRINT_VARS;


    RTNET_PROC_PRINT("Interface       API Device      Operation Mode  "
                     "Cycle\n");
    down(&tdma_nrt_lock);

    list_for_each_entry(entry, &tdma_devices, list_entry) {
        RTNET_PROC_PRINT("%-15s %-15s ", entry->rtdev->name,
                         entry->api_device.device_name);
#ifdef CONFIG_RTNET_TDMA_MASTER
        if (test_bit(TDMA_FLAG_MASTER, &entry->flags)) {
            cycle = rtos_time_to_nanosecs(&entry->cycle_period) + 500;
            do_div(cycle, 1000);
            if (test_bit(TDMA_FLAG_BACKUP_MASTER, &entry->flags))
                RTNET_PROC_PRINT("Backup Master   %ld\n",
                                 (unsigned long)cycle);
            else
                RTNET_PROC_PRINT("Master          %ld\n",
                                 (unsigned long)cycle);
        } else
#endif /* CONFIG_RTNET_TDMA_MASTER */
            RTNET_PROC_PRINT("Slave           -\n");
    }

    up(&tdma_nrt_lock);

    RTNET_PROC_PRINT_DONE;
}



int tdma_slots_proc_read(char *buf, char **start, off_t offset, int count,
                         int *eof, void *data)
{
    struct tdma_priv    *entry;
    struct tdma_slot    *slot;
    int                 i;
#ifdef CONFIG_RTNET_TDMA_MASTER
    rtos_time_t         bak_offs;
#endif
    nanosecs_t          slot_offset;
    RTNET_PROC_PRINT_VARS;


    RTNET_PROC_PRINT("Interface       "
                     "Slots (id:offset:phasing/period:size)\n");
    down(&tdma_nrt_lock);

    list_for_each_entry(entry, &tdma_devices, list_entry) {
        RTNET_PROC_PRINT("%-15s ", entry->rtdev->name);

#ifdef CONFIG_RTNET_TDMA_MASTER
        if (test_bit(TDMA_FLAG_BACKUP_MASTER, &entry->flags)) {
            rtos_time_diff(&bak_offs, &entry->backup_sync_inc,
                           &entry->cycle_period);
            slot_offset = rtos_time_to_nanosecs(&bak_offs) + 500;
            do_div(slot_offset, 1000);
            RTNET_PROC_PRINT("bak:%ld  ", (unsigned long)slot_offset);
        }
#endif /* CONFIG_RTNET_TDMA_MASTER */

        if (entry->slot_table) {
            if (down_interruptible(&entry->rtdev->nrt_sem))
                break;

            for (i = 0; i <= entry->max_slot_id; i++) {
                slot = entry->slot_table[i];
                if (!slot ||
                    ((i == DEFAULT_NRT_SLOT) &&
                     (entry->slot_table[DEFAULT_SLOT] == slot)))
                    continue;

                slot_offset = rtos_time_to_nanosecs(&slot->offset) + 500;
                do_div(slot_offset, 1000);
                RTNET_PROC_PRINT("%d:%ld:%d/%d:%d  ", i,
                                 (unsigned long)slot_offset,
                                 slot->phasing, slot->period, slot->size);
            }

            up(&entry->rtdev->nrt_sem);
        }
        RTNET_PROC_PRINT("\n");
    }

    up(&tdma_nrt_lock);

    RTNET_PROC_PRINT_DONE;
}
#endif /* CONFIG_PROC_FS */



int tdma_attach(struct rtnet_device *rtdev, void *priv)
{
    struct tdma_priv   *tdma = (struct tdma_priv *)priv;
    int                 ret;


    memset(tdma, 0, sizeof(struct tdma_priv));

    tdma->magic        = TDMA_MAGIC;
    tdma->rtdev        = rtdev;

    rtos_spin_lock_init(&tdma->lock);

    if ((ret = rtos_event_sem_init(&tdma->worker_wakeup)) != 0)
        goto err_out1;
    if ((ret = rtos_event_init(&tdma->xmit_event)) != 0)
        goto err_out2;
    if ((ret = rtos_event_init(&tdma->sync_event)) != 0)
        goto err_out3;

    ret = tdma_dev_init(rtdev, tdma);
    if (ret < 0)
        goto err_out4;

    ret = rtos_task_init(&tdma->worker_task, tdma_worker, (int)tdma, DEF_WORKER_PRIO);
    if (ret != 0)
        goto err_out5;

    RTNET_MOD_INC_USE_COUNT;

#ifdef CONFIG_PROC_FS
    down(&tdma_nrt_lock);
    list_add(&tdma->list_entry, &tdma_devices);
    up(&tdma_nrt_lock);
#endif /* CONFIG_PROC_FS */

    return 0;


  err_out5:
    tdma_dev_release(tdma);

  err_out4:
    rtos_event_delete(&tdma->sync_event);

  err_out3:
    rtos_event_delete(&tdma->xmit_event);

  err_out2:
    rtos_event_sem_delete(&tdma->worker_wakeup);

  err_out1:
    return ret;
}



int tdma_detach(struct rtnet_device *rtdev, void *priv)
{
    struct tdma_priv    *tdma = (struct tdma_priv *)priv;
    struct tdma_job     *job;
    unsigned long       flags;
    int                 ret;


    set_bit(TDMA_FLAG_SHUTDOWN, &tdma->flags);
    rtos_event_sem_signal(&tdma->worker_wakeup);

    rtos_event_broadcast(&tdma->sync_event);

    ret =  tdma_dev_release(tdma);
    if (ret < 0)
        return ret;

    rtos_event_delete(&tdma->sync_event);
    rtos_event_delete(&tdma->xmit_event);
    rtos_event_sem_delete(&tdma->worker_wakeup);

    list_for_each_entry(job, &tdma->first_job->entry, entry) {
        if (job->id >= 0)
            tdma_cleanup_slot(tdma, SLOT_JOB(job));
        else if (job->id == XMIT_RPL_CAL) {
            rtos_spin_lock_irqsave(&tdma->lock, flags);

            __list_del(job->entry.prev, job->entry.next);

            while (job->ref_count > 0) {
                rtos_spin_unlock_irqrestore(&tdma->lock, flags);
                set_current_state(TASK_UNINTERRUPTIBLE);
                schedule_timeout(HZ/10); /* wait 100 ms */
                rtos_spin_lock_irqsave(&tdma->lock, flags);
            }

            kfree_rtskb(REPLY_CAL_JOB(job)->reply_rtskb);

            rtos_spin_unlock_irqrestore(&tdma->lock, flags);
        }
    }

    rtos_task_delete(&tdma->worker_task);

    if (tdma->slot_table)
        kfree(tdma->slot_table);

#ifdef CONFIG_RTNET_TDMA_MASTER
    rtskb_pool_release(&tdma->cal_rtskb_pool);
#endif

    RTNET_MOD_DEC_USE_COUNT;

#ifdef CONFIG_PROC_FS
    down(&tdma_nrt_lock);
    list_del(&tdma->list_entry);
    up(&tdma_nrt_lock);
#endif /* CONFIG_PROC_FS */

    return 0;
}



#ifdef CONFIG_PROC_FS
struct rtmac_proc_entry tdma_proc_entries[] = {
    { name: "tdma", handler: tdma_proc_read },
    { name: "tdma_slots", handler: tdma_slots_proc_read },
    { name: NULL, handler: NULL }
};
#endif /* CONFIG_PROC_FS */

struct rtmac_disc tdma_disc = {
    name:           "TDMA",
    priv_size:      sizeof(struct tdma_priv),
    disc_type:      __constant_htons(RTMAC_TYPE_TDMA),

    packet_rx:      tdma_packet_rx,
    rt_packet_tx:   tdma_rt_packet_tx,
    nrt_packet_tx:  tdma_nrt_packet_tx,

    attach:         tdma_attach,
    detach:         tdma_detach,

    ioctls:         {
        service_name:   "RTmac/TDMA",
        ioctl_type:     RTNET_IOC_TYPE_RTMAC_TDMA,
        handler:        tdma_ioctl
    },

#ifdef CONFIG_PROC_FS
    proc_entries:   tdma_proc_entries
#endif /* CONFIG_PROC_FS */
};



int __init tdma_init(void)
{
    int ret;


    printk("RTmac/TDMA: init time division multiple access control "
           "mechanism\n");

    ret = rtmac_disc_register(&tdma_disc);
    if (ret < 0)
        return ret;

#if defined(CONFIG_RTAI_24) || defined(CONFIG_RTAI_30) || \
    defined(CONFIG_RTAI_31) || defined(CONFIG_RTAI_32)
    if (start_timer) {
        rt_set_oneshot_mode();
        start_rt_timer(0);
    }
#endif

    return 0;
}



void tdma_release(void)
{
    rtmac_disc_deregister(&tdma_disc);

#if defined(CONFIG_RTAI_24) || defined(CONFIG_RTAI_30) || \
    defined(CONFIG_RTAI_31) || defined(CONFIG_RTAI_32)
    if (start_timer)
        stop_rt_timer();
#endif

    printk("RTmac/TDMA: unloaded\n");
}



module_init(tdma_init);
module_exit(tdma_release);

MODULE_AUTHOR("Jan Kiszka");
MODULE_LICENSE("GPL");