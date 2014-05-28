/**
 * Copyright 2014 University Corporation for Atmospheric Research. All rights
 * reserved. See the the file COPYRIGHT in the top-level source-directory for
 * licensing conditions.
 *
 *   @file: mcast_down.c
 * @author: Steven R. Emmerson
 *
 * This file implements the downstream multicast LDM.
 */

#include "config.h"

#include "mcast_down.h"
#include "ldm.h"
#include "LdmBofResponse.h"
#include "ldmprint.h"
#include "log.h"
#include "pq.h"
#include "vcmtp_c_api.h"
#include "xdr.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>

/**
 * The multicast downstream LDM data-structure:
 */
struct mdl {
    pqueue*                 pq;             /* product-queue to use */
    mdl_missed_product_func missed_product; /* missed-product callback function */
    void*                   missedProdArg;  /* optional missed-product argument */
    VcmtpCReceiver*         receiver;       /* VCMTP C Receiver */
};

/**
 * Allocates space in a product-queue for a VCMTP file and set the
 * beginning-of-file response in a VCMTP file-entry.
 *
 * @param[in] pq          The product-queue.
 * @param[in] name        The name of the VCMTP file.
 * @param[in] size        Size of the XDR-encoded LDM data-product in bytes.
 * @param[in] signature   The MD5 checksum of the LDM data-product.
 * @param[in] file_entry  The corresponding VCMTP file-entry.
 * @retval    0           Success or the data-product is already in the LDM
 *                        product-queue.
 * @retval    -1          Failure. \c log_add() called.
 */
static int
allocateSpaceAndSetBofResponse(
    pqueue* const     pq,
    const char* const name,
    const size_t      size,
    const signaturet  signature,
    void* const       file_entry)
{
    int               status;
    char*             buf;
    pqe_index         index;

    if (status = pqe_newDirect(pq, size, signature, &buf, &index)) {
        if (status == PQUEUE_DUP) {
            vcmtpFileEntry_setBofResponseToIgnore(file_entry);
            status = 0;
        }
        else {
            LOG_ADD2("Couldn't allocate region for %lu-byte file \"%s\"", size,
                    name);
            status = -1;
        }
    }
    else {
        (void)vcmtpFileEntry_setBofResponse(file_entry,
                ldmBofResponse_new(buf, size, &index));
        status = 0;
    } /* region allocated in product-queue */

    return status;
}

/**
 * Sets the response attribute of a VCMTP file-entry in response to being
 * notified by the VCMTP layer about the beginning of a file. Allocates a
 * region in the LDM product-queue to receive the VCMTP file, which is an
 * XDR-encoded LDM data-product.
 *
 * @param[in,out]  obj          Pointer to the associated multicast downstream
 *                              LDM object.
 * @param[in]      file_entry   Metadata of the file in question.
 * @retval         0            Success, the transfer isn't to memory, or the
 *                              data-product is already in the LDM
 *                              product-queue.
 * @retval         -1           Failure. \c log_add() called.
 */
static int
bof_func(
    void* const    obj,
    void* const    file_entry)
{
    int            status;

    if (!vcmtpFileEntry_isMemoryTransfer(file_entry)) {
        vcmtpFileEntry_setBofResponseToIgnore(file_entry);
        status = 0;
    }
    else {
        signaturet        signature;
        const char* const name = vcmtpFileEntry_getFileName(file_entry);

        if (sigParse(name, &signature) < 0) {
            LOG_ADD1("Couldn't parse filename \"%s\" into data-product "
                    "signature", name);
            status = -1;
        }
        else {
            status = allocateSpaceAndSetBofResponse(((Mdl*)obj)->pq, name,
                    vcmtpFileEntry_getSize(file_entry), signature, file_entry);
        } /* filename is data-product signature */
    } /* transfer is to memory */

    return status;
}

/**
 * Finishes inserting a received VCMTP file into an LDM product-queue as an LDM
 * data-product.
 *
 * @param[in] pq           The LDM product-queue.
 * @param[in] index        Index of the allocated region in the product-queue.
 * @param[in] info         LDM data-product metadata.
 * @param[in] dataSize     Actual size of the data portion in bytes.
 * @return    0            Success.
 * @return    -1           Error. \c log_add() called. The allocated region in
 *                         the product-queue was released.
 */
static int
insertFileAsProduct(
    pqueue* const          pq,
    const pqe_index* const index,
    const prod_info* const info,
    const size_t           dataSize)
{
    int                    status;

    if (info->sz > dataSize) {
        LOG_ADD3("Size of LDM data-product > actual amount of data in \"%s\": "
                "LDM size=%u bytes; actual data=%lu bytes", info->ident,
                info->sz, (unsigned long)dataSize);
        status = -1;
        (void)pqe_discard(pq, *index);
    }
    else if (status = pqe_insert(pq, *index)) {
        LOG_ADD3("Couldn't finish inserting %u-byte data-product \"%s\" into "
                "product-queue: status=%d", info->sz, info->ident, status);
        status = -1;
        (void)pqe_discard(pq, *index);
    }
    return status;
}

/**
 * Accepts notification from the VCMTP layer of the complete reception of a
 * file. Finishes inserting the VCMTP file (which is an XDR-encoded
 * data-product) into the associated LDM product-queue.
 *
 * @param[in,out]  obj          Pointer to the associated multicast downstream
 *                              LDM object.
 * @param[in]      file_entry   Metadata of the VCMTP file in question.
 * @retval         0            Success, the file-transfer wasn't to memory,
 *                              or the data wasn't wanted.
 * @retval         -1           Error. \c log_add() called. The allocated space
 *                              in the LDM product-queue was released.
 */
static int
eof_func(
    void*               obj,
    const void* const   file_entry)
{
    int                 status;

    if (!vcmtpFileEntry_isWanted(file_entry) ||
            !vcmtpFileEntry_isMemoryTransfer(file_entry)) {
        status = 0;
    }
    else {
        prod_info              info;
        XDR                    xdrs;
        const size_t           fileSize = vcmtpFileEntry_getSize(file_entry);
        const void* const      bofResponse = vcmtpFileEntry_getBofResponse(file_entry);
        const pqe_index* const index = ldmBofResponse_getIndex(bofResponse);
        pqueue* const          pq = ((Mdl*)obj)->pq;

        xdrmem_create(&xdrs, (char*)ldmBofResponse_getBuf(bofResponse),
                fileSize, XDR_DECODE); /* (char*) is safe because decoding */

        if (!xdr_prod_info(&xdrs, &info)) {
            LOG_SERROR2("Couldn't decode LDM product-metadata from %lu-byte "
                    "VCMTP file \"%s\"", fileSize,
                    vcmtpFileEntry_getFileName(file_entry));
            status = -1;
            pqe_discard(pq, *index);
        }
        else {
            status = insertFileAsProduct(pq, index, &info,
                    fileSize-(xdrs.x_private-xdrs.x_base));
            xdr_free(xdr_prod_info, (char*)&info);
        } /* "info" allocated */
    } /* region in product-queue was allocated */

    return status;
}

/**
 * Accepts notification from the VCMTP layer of the missed reception of a
 * file. Queues the file for reception by other means. Returns immediately.
 *
 * @param[in,out]  obj          Pointer to the associated multicast downstream
 *                              LDM object.
 * @param[in]      fileId       Identifier of the VCMTP file that was missed.
 */
static void
missed_file_func(
    void*               obj,
    const VcmtpFileId   fileId)
{
    Mdl* const mdl = (Mdl*)obj;

    mdl->missed_product(fileId, mdl->missedProdArg);
}

/**
 * Initializes a multicast downstream LDM.
 *
 * @param[out] mdl            The multicast downstream LDM to initialize.
 * @param[in]  pq             The product-queue to use.
 * @param[in]  mcastInfo      Pointer to information on the multicast group.
 * @param[in]  missed_product Missed-product callback function.
 * @param[in]  arg            Optional argument to pass to \c missed_product.
 * @retval     0              Success.
 * @retval     LDM7_SYSTEM    System error. \c log_add() called.
 * @retval     LDM7_INVAL     @code{pq == NULL || missed_product == NULL ||
 *                            mcastInfo == NULL}. \c log_add() called.
 * @retval     LDM7_VCMTP     VCMTP error. \c log_add() called.
 */
static int
init(
    Mdl* const                          mdl,
    pqueue* const                       pq,
    const McastGroupInfo* const         mcastInfo,
    const mdl_missed_product_func       missed_product,
    void* const                         arg)
{
    int                 status;
    VcmtpCReceiver*     receiver;

    if (mdl == NULL) {
        LOG_ADD0("NULL multicast-downstream-LDM argument");
        return LDM7_INVAL;
    }
    if (pq == NULL) {
        LOG_ADD0("NULL product-queue argument");
        return LDM7_INVAL;
    }
    if (missed_product == NULL) {
        LOG_ADD0("NULL missed-product-function argument");
        return LDM7_INVAL;
    }
    if (mcastInfo == NULL) {
        LOG_ADD0("NULL multicast-group-information argument");
        return LDM7_INVAL;
    }

    status = vcmtpReceiver_new(&receiver, mcastInfo->tcpAddr,
            mcastInfo->tcpPort, bof_func, eof_func, missed_file_func,
            mcastInfo->mcastAddr, mcastInfo->mcastPort, mdl);
    if (status) {
        LOG_ADD0("Couldn't create VCMTP receiver");
        return LDM7_VCMTP;
    }

    mdl->receiver = receiver;
    mdl->pq = pq;
    mdl->missed_product = missed_product;
    mdl->missedProdArg = arg;

    return 0;
}

/**
 * Returns a new multicast downstream LDM object.
 *
 * @param[out] mdl            The pointer to be set to a new instance.
 * @param[in]  pq             The product-queue to use.
 * @param[in]  mcastInfo      Pointer to information on the multicast group.
 * @param[in]  missed_product Missed-product callback function.
 * @param[in]  arg            Optional pointer to an object to be passed to \c
 *                            missed_product().
 * @retval     0              Success.
 * @retval     LDM7_SYSTEM    System error. \c log_add() called.
 * @retval     LDM7_INVAL     @code{pq == NULL || missed_product == NULL ||
 *                            mcastInfo == NULL}. \c log_add() called.
 */
static int
mdl_new(
    Mdl* restrict* const restrict        mdl,
    pqueue* const restrict               pq,
    const McastGroupInfo* const restrict mcastInfo,
    const mdl_missed_product_func        missed_product,
    void* const                          arg)
{
    int        status;
    Mdl* const obj = LOG_MALLOC(sizeof(Mdl), "multicast downstream LDM object");

    if (NULL == obj) {
        status = LDM7_SYSTEM;
    }
    else {
        if (status = init(obj, pq, mcastInfo, missed_product, arg)) {
            free(obj);
        }
        else {
            *mdl = obj;
        }
    }

    return status;
}

/**
 * Frees the resources of a multicast downstream LDM object.
 *
 * @param[in,out] mdl   The multicast downstream LDM object.
 */
static void
mdl_free(
    Mdl* const  mdl)
{
    vcmtpReceiver_free(mdl->receiver);
    free(mdl);
}

/**
 * Executes a multicast downstream LDM. Doesn't return until the multicast
 * downstream LDM terminates.
 *
 * @param[in] mdl          The multicast downstream LDM to execute.
 * @retval    0            Success. The multicast downstream LDM terminated.
 * @retval    LDM7_INVAL   @code{mdl == NULL}. \c log_add() called.
 * @retval    -1           Failure. \c log_add() called.
 */
static int
execute(
    Mdl* const  mdl)
{
    if (NULL == mdl) {
        LOG_ADD0("NULL multicast-downstream-LDM argument");
        return LDM7_INVAL;
    }

    return vcmtpReceiver_execute(mdl->receiver);
}

/**
 * Creates and executes a multicast downstream LDM for an indefinite amount of
 * time. Returns when the multicast downstream LDM terminates.
 *
 * @param[in] mcastInfo      Pointer to multicast information.
 * @param[in] pq             The product-queue to use.
 * @param[in] missed_product Missed-product callback function.
 * @param[in] obj            Optional pointer to an object to be passed to \c
 *                           missed_product().
 * @retval    0              The multicast downstream LDM terminated
 *                           successfully.
 * @retval    LDM7_SYSTEM    System error. \c log_add() called.
 * @retval    LDM7_INVAL     @code{pq == NULL || missed_product == NULL ||
 *                           mcastInfo == NULL}. \c log_add() called.
 */
int
mdl_createAndExecute(
    const McastGroupInfo* const restrict mcastInfo,
    pqueue* const restrict               pq,
    mdl_missed_product_func              missed_product,
    void* const                          obj)
{
    Mdl*        mdl;
    int         status = mdl_new(&mdl, pq, mcastInfo, missed_product, obj);

    if (status) {
        LOG_ADD0("Couldn't create new multicast downstream LDM");
    }
    else {
        if (status = execute(mdl))
            LOG_ADD0("Failure executing multicast downstream LDM");
        mdl_free(mdl);
    } /* "mdl" allocated */

    return status;
}