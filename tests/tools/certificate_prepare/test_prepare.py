import base64
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.certificate_prepare import prepare


CERT_DER = base64.b64decode(
    "MIIBszCCARqgAwIBAgIUBhWcPocR8BJcW9WwYYheeejsJmUwDQYJKoZIhvcNAQEL"
    "BQAwEjEQMA4GA1UEAwwHVGVzdCBDQTAeFw0yNjA2MDUwMDAwMDBaFw0zNjA2MDMw"
    "MDAwMDBaMBIxEDAOBgNVBAMMB1Rlc3QgQ0EwgZ8wDQYJKoZIhvcNAQEBBQADgY0A"
    "MIGJAoGBAM6xZ2IPPyK+kTpBhkn4YKfJUxLPZ50TRDC7wVaBYn/gScCmH1woP9Nd"
    "j0FlhJLgR1v/e9LjqwhjKd6O9T4glzjXFrs72Jt2GRNno9UJdBEkOhS3Ik15xXCv"
    "cFyV/q5hhXo4WWx4n6IvrGFRWOrGRhrTvfB8OOcPuKLmRzDZAgMBAAGjUzBRMB0G"
    "A1UdDgQWBBRBiL8hh9UbTR9ywBNjgECmeZbHZjAfBgNVHSMEGDAWgBRBiL8hh9Ub"
    "TR9ywBNjgECmeZbHZjAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4GB"
    "AH5VqqTRoBlCqKPkik/JYqVQtBkFfzvNXeWt7D5DxUpESt7n61MUuhd4USoVLCJW"
    "kP29lmvup67xv5KoODzs5V+c9kcJQjjRVe74+mSZ6tpCylJp1iVi+hixs93Tw/bo"
    "l9aJnj+ow+arqo8OdM3CcgMZk4EdOO3MdP6l"
)


def certdata_object(label, der_bytes, trusted=True):
    octal = "".join(f"\\{byte:03o}" for byte in der_bytes)
    trust_value = (
        "CKT_NSS_TRUSTED_DELEGATOR"
        if trusted
        else "CKT_NSS_NOT_TRUSTED"
    )
    return f"""
CKA_CLASS CK_OBJECT_CLASS CKO_CERTIFICATE
CKA_LABEL UTF8 "{label}"
CKA_VALUE MULTILINE_OCTAL
{octal}
END

CKA_CLASS CK_OBJECT_CLASS CKO_NSS_TRUST
CKA_LABEL UTF8 "{label}"
CKA_TRUST_SERVER_AUTH CK_TRUST {trust_value}
"""


class CertdataConversionTests(unittest.TestCase):
    def test_convert_certdata_to_pem_keeps_trusted_server_auth_ca(self):
        pem = prepare.convert_certdata_to_pem(certdata_object("Test CA", CERT_DER))

        self.assertIn("-----BEGIN CERTIFICATE-----", pem)
        self.assertIn("-----END CERTIFICATE-----", pem)
        self.assertIn(base64.encodebytes(CERT_DER).decode("ascii").splitlines()[0], pem)

    def test_convert_certdata_to_pem_rejects_when_no_trusted_server_auth_ca_exists(self):
        with self.assertRaisesRegex(prepare.PrepareError, "No trusted server-auth"):
            prepare.convert_certdata_to_pem(certdata_object("Ignored CA", CERT_DER, trusted=False))


class OutputValidationTests(unittest.TestCase):
    def test_validate_output_rejects_empty_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "bundle_ca.bin"
            output.write_bytes(b"")

            with self.assertRaisesRegex(prepare.PrepareError, "empty"):
                prepare.validate_output(output, max_size=10)

    def test_validate_output_rejects_oversized_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "bundle_ca.bin"
            output.write_bytes(b"123456")

            with self.assertRaisesRegex(prepare.PrepareError, "exceeds"):
                prepare.validate_output(output, max_size=5)


class MainFlowTests(unittest.TestCase):
    def test_default_certdata_url_uses_mozilla_raw_file_default_endpoint(self):
        self.assertEqual(
            prepare.DEFAULT_CERTDATA_URL,
            "https://hg.mozilla.org/mozilla-central/raw-file/default/"
            "security/nss/lib/ckfw/builtins/certdata.txt",
        )

    def test_main_uses_local_certdata_override_and_moves_generated_bundle(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            certdata = tmp_path / "certdata.txt"
            output = tmp_path / "out" / "bundle_ca.bin"
            gen_script = tmp_path / "gen_crt_bundle.py"
            certdata.write_text(certdata_object("Test CA", CERT_DER), encoding="utf-8")
            gen_script.write_text("unused", encoding="utf-8")

            def fake_run(command, cwd, check):
                self.assertEqual(Path(cwd), tmp_path / "work")
                self.assertEqual(command[0], prepare.sys.executable)
                self.assertEqual(command[1], str(gen_script))
                self.assertIn("--input", command)
                self.assertIn("--max-certs", command)
                (Path(cwd) / "x509_crt_bundle").write_bytes(b"bundle")
                return mock.Mock(returncode=0)

            with mock.patch.object(prepare.subprocess, "run", side_effect=fake_run):
                exit_code = prepare.main([
                    "--certdata", str(certdata),
                    "--gen-crt-bundle", str(gen_script),
                    "--work-dir", str(tmp_path / "work"),
                    "--output", str(output),
                    "--max-size", "16",
                ])

            self.assertEqual(exit_code, 0)
            self.assertEqual(output.read_bytes(), b"bundle")
            self.assertTrue((tmp_path / "work" / "mozilla_ca.pem").exists())


if __name__ == "__main__":
    unittest.main()
