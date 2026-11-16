/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Tokenized log format strings (shim/log_token.h). INFO: kept in the ELF for
 * scripts/esp32_logtok_db.py, never allocated, so never in the image. At
 * address 0, each string's address is its offset: the token a record carries. */
SECTIONS
{
    .helix_logfmt 0 (INFO) :
    {
        KEEP(*(.helix_logfmt.*))
    }
}
