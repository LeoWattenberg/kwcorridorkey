import pytest

from corridorkey_worker.settings import WorkerSettings


def test_settings_defaults_validate():
    WorkerSettings().validate()


def test_settings_reject_unknown_key():
    with pytest.raises(ValueError, match="Unknown settings"):
        WorkerSettings.from_payload({"unknown": True})


def test_settings_reject_bad_output_mode():
    with pytest.raises(ValueError, match="output_mode"):
        WorkerSettings.from_payload({"output_mode": "bad"})

