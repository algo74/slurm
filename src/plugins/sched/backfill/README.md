# `sched/backfill` plugin 

Modified backfill plugin keeping the same name


## Modifications from original

*	The basic idea for finding earliest time a job can run:
    1.	find earliest time licenses for the job are available
        -	“lic_tracker_p lt” is a “step function”-like datastructure that keep track of usage of licenses.
        -	Slurm uses a similar approach but it keeps track of nodes using a stack bitmaps. It looks quite complicated, so I just let Slurm track nodes.
    2.	find earliest time from time obtained in #1 when reservations allow the job to run. If the time is not the same, repeat from #1 starting from this time
    3.	find earliest time starting from time obtained in #2 when nodes for the job are available. If the time is not the same, repeat from #1 starting from this time

*	the current resource utilization (although is used here) is actually updated in the job_submit plugin


----
Copyright (C) 2024 Alexander Goponenko, University of Central Florida.
Distributed with no warranty under the GNU General Public License.
See the GNU General Public License for more details.