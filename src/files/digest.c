#include "files/digest.h"

#include <openssl/evp.h>

bool ph_file_digest(FILE *file, char hex[65], bool (*work)(void *), void *user)
{
    static const char digits[] = "0123456789abcdef";
    if (!file || fseek(file, 0, SEEK_SET) != 0)
        return false;
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    bool ok = context && EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
    unsigned char data[65536], digest[32];
    unsigned length;
    size_t n, since_work = 0;
    while (ok && (n = fread(data, 1, sizeof data, file)) != 0) {
        ok = EVP_DigestUpdate(context, data, n) == 1;
        since_work += n;
        if (ok && work && since_work >= sizeof data * 4) {
            ok = work(user);
            since_work = 0;
        }
    }
    ok = ok && !ferror(file) && EVP_DigestFinal_ex(context, digest, &length) == 1 &&
         length == sizeof digest && fseek(file, 0, SEEK_SET) == 0;
    EVP_MD_CTX_free(context);
    if (!ok)
        return false;
    for (unsigned i = 0; i < length; i++) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[64] = 0;
    return true;
}
