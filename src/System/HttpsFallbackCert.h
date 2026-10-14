#pragma once

// Baked-in CA/server cert so boot does not ECDSA-mint while AIRSTEP is
// joining. Runtime mint remakes the CA every boot (NVS is full) and the
// PTA stall kills STA unicast, so https://showbox.local/ never answers.
static const char kHttpsFallbackCert[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICIDCCAcWgAwIBAgIUZfybwLw9B4owJD9hHNCkVqpxw7wwCgYIKoZIzj0EAwIw\n"
    "QDEWMBQGA1UEAwwNc2hvd2JveC5sb2NhbDETMBEGA1UECgwKU2hvd2JveE1vZDER\n"
    "MA8GA1UECwwIZmFsbGJhY2swHhcNMjYxMDA5MjA1OTMyWhcNNDkwNTExMjA1OTMy\n"
    "WjBAMRYwFAYDVQQDDA1zaG93Ym94LmxvY2FsMRMwEQYDVQQKDApTaG93Ym94TW9k\n"
    "MREwDwYDVQQLDAhmYWxsYmFjazBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABOvm\n"
    "N9MJhsDhjQlCpirn+DwhUjS5y6/PPBxl1WnCOPnqXarer2zn9Py1674UQUGC+a7M\n"
    "JKM7z6vlPbk60hZgJzGjgZwwgZkwHQYDVR0OBBYEFHr2B+7UBh1+jRpzNqYBBoBJ\n"
    "nBe4MB8GA1UdIwQYMBaAFHr2B+7UBh1+jRpzNqYBBoBJnBe4MCEGA1UdEQQaMBiC\n"
    "DXNob3dib3gubG9jYWyCB3Nob3dib3gwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8B\n"
    "Af8EBAMCAaYwEwYDVR0lBAwwCgYIKwYBBQUHAwEwCgYIKoZIzj0EAwIDSQAwRgIh\n"
    "AMT54ZnTAV3LVMIcgci26ETgKe/zVm8Tlk50cVTnfQsFAiEAmqpliSezdQfhDp/T\n"
    "rsYWphvvBvQR//IxbQtE4u3HQyE=\n"
    "-----END CERTIFICATE-----\n";

static const char kHttpsFallbackKey[] =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEIIR3KOv0dQ9EzDiS3xub0AFLYjldyYvDS8ihyvtdUrYOoAoGCCqGSM49\n"
    "AwEHoUQDQgAE6+Y30wmGwOGNCUKmKuf4PCFSNLnLr888HGXVacI4+epdqt6vbOf0\n"
    "/LXrvhRBQYL5rswkozvPq+U9uTrSFmAnMQ==\n"
    "-----END EC PRIVATE KEY-----\n";
