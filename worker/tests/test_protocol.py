from corridorkey_worker.protocol import roundtrip_for_tests


def test_protocol_roundtrip():
    message = {"id": 7, "command": "hello", "payload": {"value": "ok"}}
    assert roundtrip_for_tests(message) == message

