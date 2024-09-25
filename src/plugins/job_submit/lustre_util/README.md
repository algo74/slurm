
# `job_submit/lustre_util` plugin

-	starts a dedicated tread that periodically gets total usage from the remote service
    -	(via TCP) can be configured with environmental variable `VINSNL_SERVER`

-	when a job is submitted 
    -	gets “classification tag” (`variety_id`)from a remote service (via TCP) can be configured with environmental variable `VINSNL_SERVER`
    -	stores the `variety_id` in the comment field
    -	gets resource requirement prediction from a remote service (via TCP); add them to the job (but only if user has not set them; the user’s requirements are not overwritten)

----
Copyright (C) 2024 Alexander Goponenko, University of Central Florida.
Distributed with no warranty under the GNU General Public License.
See the GNU General Public License for more details.