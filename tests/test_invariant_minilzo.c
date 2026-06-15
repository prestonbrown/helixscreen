#include <check.h>
#include <stdlib.h>
#include <string.h>

#include "lib/minilzo/minilzo.h"

START_TEST(test_decompress_rejects_oversized_output)
{
    /* Invariant: Decompression into a small buffer must never write beyond
       the declared output length — it must return an error instead. */

    /* Craft compressed payloads that claim to decompress to more data than
       the output buffer can hold. We compress valid data, then try to
       decompress into a too-small buffer. */
    unsigned char src[256];
    unsigned char compressed[256 + 256/16 + 64 + 3];
    unsigned char out_buf[32]; /* intentionally small output buffer */
    lzo_uint src_len, comp_len, out_len;
    int r;

    /* Initialize LZO */
    ck_assert_int_eq(lzo_init(), LZO_E_OK);

    /* Payload 1: Valid input that compresses to something larger than out_buf */
    memset(src, 'A', 128);
    src_len = 128;
    comp_len = sizeof(compressed);
    unsigned char wrkmem[LZO1X_1_MEM_COMPRESS];
    r = lzo1x_1_compress(src, src_len, compressed, &comp_len, wrkmem);
    ck_assert_int_eq(r, LZO_E_OK);

    /* Try decompressing into a buffer that is too small (32 < 128) */
    out_len = sizeof(out_buf);
    r = lzo1x_decompress_safe(compressed, comp_len, out_buf, &out_len, NULL);
    /* Must NOT return OK — output would overflow */
    ck_assert_int_ne(r, LZO_E_OK);

    /* Payload 2: Boundary — output buffer exactly matches decompressed size */
    memset(src, 'B', 32);
    src_len = 32;
    comp_len = sizeof(compressed);
    r = lzo1x_1_compress(src, src_len, compressed, &comp_len, wrkmem);
    ck_assert_int_eq(r, LZO_E_OK);
    out_len = sizeof(out_buf); /* exactly 32 bytes */
    r = lzo1x_decompress_safe(compressed, comp_len, out_buf, &out_len, NULL);
    ck_assert_int_eq(r, LZO_E_OK);
    ck_assert_uint_eq(out_len, 32);

    /* Payload 3: Large input (10x buffer) must be rejected */
    memset(src, 'C', 256);
    src_len = 256;
    comp_len = sizeof(compressed);
    r = lzo1x_1_compress(src, src_len, compressed, &comp_len, wrkmem);
    ck_assert_int_eq(r, LZO_E_OK);
    out_len = sizeof(out_buf); /* 32 < 256 */
    r = lzo1x_decompress_safe(compressed, comp_len, out_buf, &out_len, NULL);
    ck_assert_int_ne(r, LZO_E_OK);
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_decompress_rejects_oversized_output);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}