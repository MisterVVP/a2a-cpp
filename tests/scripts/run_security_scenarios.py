#!/usr/bin/env python3
"""Run SDK security scenarios with local credentials, CA and TLS termination."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid

from run_binding_scenarios import reserve_ports, wait_until_ready, READY_TIMEOUT, SCENARIO_TIMEOUT

ALICE_TOKEN = "security-fixture-alice"
BOB_TOKEN = "security-fixture-bob"
TASK_CREDENTIAL = "security-fixture-task-credential"
KEY_ID = "security-fixture-card-key"
PUBLIC_GRPC_TARGET_ENV = "A2A_TCK_PUBLIC_GRPC_TARGET"
VALID_FROM = "20200101000000Z"
VALID_UNTIL = "21000101000000Z"
EXPIRED_UNTIL = "20210101000000Z"


def openssl(directory, *arguments):
    subprocess.run(["openssl", *arguments], cwd=directory, check=True, capture_output=True)


def fixtures(directory):
    openssl(directory, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key",
            "-out", "ca.pem", "-days", "30000", "-subj", "/CN=SDK fixture CA")
    openssl(directory, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "other-ca.key",
            "-out", "other-ca.pem", "-days", "30000", "-subj", "/CN=Other SDK fixture CA")
    openssl(directory, "req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", "server.key",
            "-out", "server.csr", "-subj", "/CN=localhost")
    (directory / "index.txt").touch()
    (directory / "serial").write_text("1000\n", encoding="utf-8")
    (directory / "ca.conf").write_text(f"""[ca]
default_ca = fixture
[fixture]
database = {directory}/index.txt
serial = {directory}/serial
new_certs_dir = {directory}
certificate = {directory}/ca.pem
private_key = {directory}/ca.key
default_md = sha256
policy = fixture_policy
unique_subject = no
x509_extensions = server
[fixture_policy]
commonName = supplied
[server]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = DNS:localhost
""", encoding="utf-8")
    for name, until in (("server.pem", VALID_UNTIL), ("expired.pem", EXPIRED_UNTIL)):
        openssl(directory, "ca", "-batch", "-notext", "-config", "ca.conf", "-in", "server.csr",
                "-out", name, "-startdate", VALID_FROM, "-enddate", until)
    openssl(directory, "genpkey", "-algorithm", "ED25519", "-out", "card.key")
    openssl(directory, "pkey", "-in", "card.key", "-pubout", "-out", "card.pub")
    for key in directory.glob("*.key"):
        key.chmod(0o600)


def stop(process):
    if process is None:
        return
    process.terminate()
    try:
        process.wait(timeout=READY_TIMEOUT)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def run_scenario(args, environment, directory, mode):
    env = dict(environment)
    port = reserve_ports()
    env["A2A_SECURITY_SUT_PORT"] = str(port)
    env["A2A_SECURITY_MODE"] = mode
    env["A2A_TCK_CREDENTIAL_DIRECTORY"] = str(directory / "credentials")
    env["A2A_TCK_SIGNING_KEY_FILE"] = str(directory / "card.key")
    env["A2A_TCK_SIGNING_KEY_ID"] = KEY_ID
    env["A2A_SECURITY_CARD_PUBLIC_KEY"] = str(directory / "card.pub")
    proxy = None
    if mode != "plaintext":
        tls_port = reserve_ports()
        certificate = directory / ("expired.pem" if mode == "expired" else "server.pem")
        env["A2A_TCK_TLS_CERT_FILE"] = str(certificate)
        env["A2A_TCK_TLS_KEY_FILE"] = str(directory / "server.key")
        env["A2A_SECURITY_CA_FILE"] = str(directory / ("other-ca.pem" if mode == "untrusted" else "ca.pem"))
        host = "127.0.0.1" if mode == "hostname_mismatch" else "localhost"
        env["A2A_SECURITY_TLS_HOST"] = host
        env["A2A_SECURITY_HTTPS_PORT"] = str(tls_port)
        env["A2A_TCK_PUBLIC_HTTPS_BASE"] = f"https://{host}:{tls_port}"
        env[PUBLIC_GRPC_TARGET_ENV] = f"{host}:{port + 1}"
        config = directory / "nginx.conf"
        config.write_text(f"""pid {directory}/nginx.pid;
error_log {directory}/nginx-error.log;
events {{}}
http {{
  access_log off;
  server {{
    listen 127.0.0.1:{tls_port} ssl;
    ssl_certificate {certificate};
    ssl_certificate_key {directory}/server.key;
    ssl_protocols TLSv1.3;
    location / {{
      proxy_pass http://127.0.0.1:{port};
      proxy_http_version 1.1;
      proxy_buffering off;
      proxy_read_timeout 30s;
    }}
  }}
}}
""", encoding="utf-8")
    print(f"SDK security: backend={args.backend}, mode={mode}", flush=True)
    with tempfile.TemporaryFile(mode="w+") as log:
        sut = subprocess.Popen([str(args.sut), f"127.0.0.1:{port}"], env=env, stdout=log, stderr=log)
        try:
            wait_until_ready(sut, port)
            if mode != "plaintext":
                proxy = subprocess.Popen(["nginx", "-c", str(directory / "nginx.conf"), "-g", "daemon off;"],
                                         stdout=log, stderr=log)
                wait_until_ready(proxy, tls_port)
            if args.independent_sdk:
                sdk_filter = "*Tls*" if mode not in ("plaintext", "trusted") else "*"
                sdk_result = subprocess.run([str(args.independent_sdk), f"--gtest_filter={sdk_filter}"], env=env,
                                            timeout=SCENARIO_TIMEOUT, check=False)
                if sdk_result.returncode:
                    return sdk_result.returncode
            test_filter = "*Tls*" if mode not in ("plaintext", "trusted") else "*"
            return subprocess.run([str(args.scenarios), f"--gtest_filter={test_filter}"], env=env,
                                  timeout=SCENARIO_TIMEOUT, check=False).returncode
        finally:
            stop(proxy)
            stop(sut)
            log.seek(0)
            print(log.read(), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sut", required=True, type=Path)
    parser.add_argument("--scenarios", required=True, type=Path)
    parser.add_argument("--independent-sdk", type=Path)
    parser.add_argument("--backend", required=True, choices=("inmemory", "postgres"))
    args = parser.parse_args()
    environment = dict(os.environ)
    environment["A2A_TCK_PROFILE"] = "security"
    environment["A2A_TCK_AUTH_TOKEN"] = ""
    environment["A2A_TCK_ALICE_TOKEN"] = ALICE_TOKEN
    environment["A2A_TCK_BOB_TOKEN"] = BOB_TOKEN
    environment["A2A_TCK_TASK_CREDENTIAL"] = TASK_CREDENTIAL
    environment["A2A_TCK_STORE_BACKEND"] = args.backend
    environment["A2A_TCK_EXTENDED_AGENT_CARD_MODE"] = "configured"
    for name in ("A2A_TCK_TLS_CERT_FILE", "A2A_TCK_TLS_KEY_FILE", "A2A_TCK_PUBLIC_HTTPS_BASE",
                 PUBLIC_GRPC_TARGET_ENV):
        environment.pop(name, None)
    schema = f"security_{uuid.uuid4().hex}"
    if args.backend == "postgres":
        dsn = environment.get("A2A_TEST_POSTGRES_DSN")
        if not dsn:
            print("SKIP: A2A_TEST_POSTGRES_DSN is required")
            return 77
        environment["A2A_TCK_POSTGRES_DSN"] = dsn
        environment["A2A_TCK_POSTGRES_SCHEMA"] = schema
    if not shutil.which("nginx") or not shutil.which("openssl"):
        raise RuntimeError("SDK security scenarios require nginx and openssl")
    try:
        with tempfile.TemporaryDirectory(prefix="sdk-security-") as scratch:
            directory = Path(scratch)
            (directory / "credentials").mkdir(mode=0o700)
            fixtures(directory)
            for mode in ("plaintext", "trusted", "untrusted", "expired", "hostname_mismatch"):
                result = run_scenario(args, environment, directory, mode)
                if result:
                    return result
        return 0
    finally:
        if args.backend == "postgres" and shutil.which("psql"):
            subprocess.run(["psql", environment["A2A_TCK_POSTGRES_DSN"], "-v", "ON_ERROR_STOP=1", "-c",
                            f'DROP SCHEMA "{schema}" CASCADE'], check=True, capture_output=True)


if __name__ == "__main__":
    raise SystemExit(main())
