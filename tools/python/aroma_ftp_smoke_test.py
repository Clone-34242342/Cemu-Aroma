#!/usr/bin/env python3
"""Verify an anonymous FTPiiU upload/download without leaving a test file."""
import argparse
from ftplib import FTP
import hashlib
from io import BytesIO
import json
import secrets
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2121)
    parser.add_argument("--directory", default="/fs/vol/external01/wiiu/apps")
    args = parser.parse_args()
    path = args.directory.rstrip("/") + "/.cemu-ftp-test-" + uuid.uuid4().hex
    payload = secrets.token_bytes(4096)
    with FTP() as ftp:
        ftp.connect(args.host, args.port, timeout=15)
        ftp.login()
        try:
            ftp.storbinary("STOR " + path, BytesIO(payload))
            received = BytesIO()
            ftp.retrbinary("RETR " + path, received.write)
            result = {"size": len(payload), "sha256": hashlib.sha256(payload).hexdigest(),
                      "received_sha256": hashlib.sha256(received.getvalue()).hexdigest(),
                      "success": received.getvalue() == payload}
        finally:
            ftp.delete(path)
    print(json.dumps(result, indent=2))
    return 0 if result["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
