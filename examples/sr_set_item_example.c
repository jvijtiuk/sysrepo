/**
 * @file sr_set_item_example.c
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 * @brief Example usage of sr_set_item_example function.
 *
 * @copyright
 * Copyright 2016 Cisco Systems, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "sysrepo.h"

int
main(int argc, char **argv)
{
    sr_conn_ctx_t *conn = NULL;
    sr_session_ctx_t *sess = NULL;
    sr_val_t value = { 0 };
    int rc = SR_ERR_OK;

    /* connect to sysrepo */
    rc = sr_connect("app3", true, &conn);
    if (SR_ERR_OK != rc) {
        goto cleanup;
    }

    /* start session */
    rc = sr_session_start(conn, NULL, SR_DS_STARTUP, &sess);
    if (SR_ERR_OK != rc) {
        goto cleanup;
    }

    /* set 'prefix-length' leaf inside of the 'address' list entry with key '172.16.0.1'
       (list entry will be automatically created if it does not exist) */
    value.type = SR_UINT8_T;
    value.data.uint8_val = 24;
    rc = sr_set_item(sess, "/ietf-interfaces:interfaces/interface[name='gigaeth0']/ietf-ip:ipv4/address[ip='172.16.0.1']/prefix-length",
            &value, SR_EDIT_DEFAULT);
    if (SR_ERR_OK != rc) {
        printf("Error by sr_set_item: %s\n", sr_strerror(rc));
        goto cleanup;
    }

    /* commit the changes */
    rc = sr_commit(sess, NULL, NULL);
    if (SR_ERR_OK != rc) {
        printf("Error by sr_commit: %s\n", sr_strerror(rc));
        goto cleanup;
    }

cleanup:
    if (NULL != sess) {
        sr_session_stop(sess);
    }
    if (NULL != conn) {
        sr_disconnect(conn);
    }
    return rc;
}