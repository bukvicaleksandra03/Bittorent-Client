# Self-Signed Certificate Generation

Command used to generate the certificate:

```bash
openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt -days 36500 -nodes -subj "/CN=localhost"
```

## Flag Breakdown

### `openssl`

The OpenSSL command-line tool. This is the Swiss Army knife of cryptography -- handles certificates, keys, encryption, hashing, and more.

### `req`

The subcommand for certificate request operations. Normally it creates CSRs (Certificate Signing Requests) that you'd send to a CA. But combined with `-x509`, it skips the CA step and directly outputs a certificate.

### `-x509`

Instead of generating a CSR, produce a self-signed certificate directly. Without this flag, the output would be a CSR (a "please sign me" request), not a usable certificate. With it, you're acting as your own CA -- you sign it yourself with the key you're generating.

### `-newkey rsa:2048`

Generate a brand new private key using the RSA algorithm with a 2048-bit key size. This is the key pair used for:

- **Private key** -- kept secret, used to prove identity
- **Public key** -- embedded in the certificate, given to clients

2048 bits is the minimum considered secure today. You could use `rsa:4096` for stronger security, but for testing it makes no practical difference.

### `-keyout server.key`

Write the generated private key to the file `server.key`. This file is in PEM format (base64-encoded, wrapped with `-----BEGIN PRIVATE KEY-----` / `-----END PRIVATE KEY-----`). On a real server, this file must be kept secret and have restrictive permissions (e.g., `chmod 600`).

### `-out server.crt`

Write the generated self-signed certificate to the file `server.crt`. Also PEM format (`-----BEGIN CERTIFICATE-----` / `-----END CERTIFICATE-----`). This is what gets sent to clients during the TLS handshake. It contains:

- The public key
- The subject name (who the cert belongs to)
- The issuer name (who signed it -- in self-signed, same as subject)
- Validity dates (not before / not after)
- The CA's digital signature over all of the above

### `-days 36500`

The certificate is valid for 36,500 days (~100 years) from now. After this period, any client checking the certificate's `notAfter` field will reject it as expired. For testing, this large value means you never have to worry about regenerating.

### `-nodes`

Stands for "no DES" (no encryption on the private key). Without this flag, OpenSSL prompts you for a passphrase and encrypts `server.key` with it. You'd then need to enter that passphrase every time the server loads the key. For testing, skipping the passphrase is convenient. In production, you might want the passphrase for extra protection.

### `-subj "/CN=localhost"`

Set the certificate's subject field directly on the command line, skipping the interactive prompts. The subject identifies who the certificate belongs to. The format uses LDAP-style distinguished name fields:

- **CN** = Common Name -- the hostname the cert is for. Here it's `localhost`, which matches when your test client connects to `127.0.0.1` / `localhost`.

Other fields you could include (not needed for testing):

- `/O=MyOrg` -- Organization
- `/C=US` -- Country
- `/ST=California` -- State
- `/L=SanJose` -- Locality

## What Happens When You Run It

1. OpenSSL generates a 2048-bit RSA key pair (public + private)
2. It builds a certificate containing the public key, the subject `/CN=localhost`, and validity dates (today through today + 36500 days)
3. It signs the certificate with the private key it just generated (self-signed)
4. It writes the private key to `server.key`
5. It writes the signed certificate to `server.crt`

The result is two files that together let you run a TLS server on localhost for testing purposes.


## Suggestion for improvement

- Create a CSR and submit it to a real CA (Let's Encrypt for example). 
