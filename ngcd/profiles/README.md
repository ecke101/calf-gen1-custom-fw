# Additional camera profiles

These profiles extend the stock files in `/local` and must be installed with
the matching `ngcd` and UI builds.

For full-FOV VR180 5K60, install:

| Repository file | Camera destination |
|---|---|
| `ngcd-vr180-5k60.yaml` | `/local/ngcd-vr180-5k60.yaml` |
| `vr180_5120x2560.pto` | `/local/vr180/vr180_5120x2560.pto` |

Verify byte counts and hashes before publishing either file. During temporary
testing, download each asset to a unique `/tmp` path, create its destination
only if it does not already exist, then bind mount the checked temporary file
over that destination. Never stack bind mounts. A reboot removes the mounts;
remove newly created empty destination placeholders when rolling back.

The 5K60 graph keeps the calibrated 3360x2880 input from each lens, runs both
IMX577 sensors at 3520x3040/60, and changes only the stitched output sampling
to 5120x2560. This preserves the VR180 field of view.

Hardware validation on 2026-08-13 confirmed 59.99 recorded fps at an average
148.97 Mbps after shortening the backend service-loop wait to 5 ms. Deploy the
matching backend; the older 20 ms loop recorded only about 44 fps even though
the sensors and encoder were configured for 60.
