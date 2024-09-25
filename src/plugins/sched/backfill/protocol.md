Connection protocol for SLURM-LDMS middleman server
===================================================

General
--------------------------------

* JSON request:  {“req_id: “...”, “type”: ”...”, ...}\n
* JSON response: {“req_id: “...”, “status”: ”error|OK|ACK|not implemented”, ...}\n

"type”: ”usage”
--------------------------------

* Request: 
  - “request”: [”lustre”, ...] -- ignored by the middlemant server

* Response: 
  - “status”: ”OK”
  - “response” : {”lustre” : “<int>”, ... }
  
"type" : "variety_id"
--------------------------------

* Request:
  - "UID" : "..."
  - "GID" : "..."
  - "min_nodes" : "<int>"
  - "max_nodes" : "<int>"
  
* Response: 
  - “status”: ”OK”, 
  - “variety_id”: ”...”

### "type" : "variety_id/manual"

* Request:
  - "variety_name" : "..."

### "type" : "variety_id/auto"

* Request: 
  - “script_args”: [“<arg0>”, “<arg1>”, ...], 
  - “script_name”: ”...”


“type”: ”process_job”
--------------------------------

* Request: “variety_id”: ”...”, “job_id”: ”<int>”
* Response: “status”: ”ACK”

“type”: ”job_utilization”
--------------------------------

* Request: 
  - “variety_id”: ”...”
  - "nodes" : "<int>"
  
* Response: 
  - “status”: ”OK”, 
  - “response” : {”lustre” : “<int>”, ...}


----
Copyright (C) 2020 Alexander Goponenko, University of Central Florida.
Distributed with no warranty under the GNU General Public License.
See the GNU General Public License for more details.