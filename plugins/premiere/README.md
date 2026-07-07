# CorridorKey Premiere Plugin

This target builds an Adobe effect plugin (`CorridorKeyPremiere.aex`) when an Adobe After Effects/Premiere-compatible SDK is supplied.

Set one of:

- `ADOBE_AE_SDK_ROOT`
- `ADOBE_PREMIERE_SDK_ROOT`

The effect exposes CorridorKey parameters and uses the shared worker bridge. Where the host supports a layer/matte parameter, that layer is used as the coarse alpha hint. The implementation also exposes a sidecar path parameter for workflows where layer input is not available.

Set `CORRIDORKEY_WORKER_PYTHON` to the Python executable printed by `scripts/install_corridorkey.py`.

