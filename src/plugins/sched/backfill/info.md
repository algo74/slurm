Options and their effect
=========================

REMOTE_ESTIMATES_ENABLED (probably default)
------------------------

- Obtain estimates from remote

REMOTE_ESTIMATES_DISABLED (probably not implemented)
-------------------------

- Ignore estimate requests
  - return unmodified (must be zeros)
> TODO: correlate with job_submit plugin

BACKFILL_LICENSES_AWARE (default)
------------------------

- Enforce limits on licenses
- Use estimates if they are not zero
- Clip estimates


BACKFILL_LICENSES_TWO_GROUP
----------------------------

- Enforce limits on licenses
- Use estimates if they are not zero 
  - Clip estimates
- Determine R_star and R_bar
- Enforce R_prime "target"


HOWTO IMPLEMENT DIFFERENT SCHDULING STRATEGIES
==============================================

Baseline
--------

- Configure "Analytical Services" to return zero estimates and zero current usage
- Configure "Backfill" to be "Aware"

I/O Aware
---------

- Configure "Analytical Services" to return non-zero estimates and current usage
- Configure "Backfill" to be "Aware"

Workload-adaptive
-----------------

- Configure "Analytical Services" to return non-zero estimates and current usage
- Configure "Backfill" to be "Two-group"


NOTES and TODOS
=========================

### `r_used`, `used`, or other?

- No job usage prediction: max(r_used, used) is assumed to be the currently allocated number of licenses

- I/O aware with predictions: max(r_used, used + sum of all predictions)

- Two-group approximation: 
  - same as I/O aware for limits
  - sum of all calculated usages for "star" target (which should correspond to "used + sum of all predictions", but calculated independently)

### clipping estimates

- clip when testing licenses against limits
  > but not when allocating licenses (we assume that allocating more licenses than the limit is allowed)

- don't clip when calculating r_star and when testing against "star" target
  > we assume that because target can be exceeded, no need to clip


-----
Copyright (C) 2020 Alexander Goponenko, University of Central Florida.
Distributed with no warranty under the GNU General Public License.
See the GNU General Public License for more details.