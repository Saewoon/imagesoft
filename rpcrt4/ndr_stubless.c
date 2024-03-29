/*
 * NDR -Oi,-Oif,-Oicf Interpreter
 *
 * Copyright 2001 Ove K�ven, TransGaming Technologies
 * Copyright 2003-5 Robert Shearman (for CodeWeavers)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * TODO:
 *  - Pipes
 *  - Some types of binding handles
 */

#include "config.h"
#include "wine/port.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"

#include "objbase.h"
#include "rpc.h"
#include "rpcproxy.h"
#include "ndrtypes.h"

#include "wine/debug.h"
#include "wine/rpcfc.h"

#include "ndr_misc.h"
#include "cpsf.h"

WINE_DEFAULT_DEBUG_CHANNEL(rpc);

#define NDR_TABLE_MASK 127

static inline void call_buffer_sizer(PMIDL_STUB_MESSAGE pStubMsg, unsigned char *pMemory, PFORMAT_STRING pFormat)
{
    NDR_BUFFERSIZE m = NdrBufferSizer[pFormat[0] & NDR_TABLE_MASK];
    if (m) m(pStubMsg, pMemory, pFormat);
    else
    {
        FIXME("format type 0x%x not implemented\n", pFormat[0]);
        RpcRaiseException(RPC_X_BAD_STUB_DATA);
    }
}

static inline unsigned char *call_marshaller(PMIDL_STUB_MESSAGE pStubMsg, unsigned char *pMemory, PFORMAT_STRING pFormat)
{
    NDR_MARSHALL m = NdrMarshaller[pFormat[0] & NDR_TABLE_MASK];
    if (m) return m(pStubMsg, pMemory, pFormat);
    else
    {
        FIXME("format type 0x%x not implemented\n", pFormat[0]);
        RpcRaiseException(RPC_X_BAD_STUB_DATA);
        return NULL;
    }
}

static inline unsigned char *call_unmarshaller(PMIDL_STUB_MESSAGE pStubMsg, unsigned char **ppMemory, PFORMAT_STRING pFormat, unsigned char fMustAlloc)
{
    NDR_UNMARSHALL m = NdrUnmarshaller[pFormat[0] & NDR_TABLE_MASK];
    if (m) return m(pStubMsg, ppMemory, pFormat, fMustAlloc);
    else
    {
        FIXME("format type 0x%x not implemented\n", pFormat[0]);
        RpcRaiseException(RPC_X_BAD_STUB_DATA);
        return NULL;
    }
}

static inline void call_freer(PMIDL_STUB_MESSAGE pStubMsg, unsigned char *pMemory, PFORMAT_STRING pFormat)
{
    NDR_FREE m = NdrFreer[pFormat[0] & NDR_TABLE_MASK];
    if (m) m(pStubMsg, pMemory, pFormat);
}

static inline unsigned long call_memory_sizer(PMIDL_STUB_MESSAGE pStubMsg, PFORMAT_STRING pFormat)
{
    NDR_MEMORYSIZE m = NdrMemorySizer[pFormat[0] & NDR_TABLE_MASK];
    if (m)
    {
        unsigned char *saved_buffer = pStubMsg->Buffer;
        unsigned long ret;
        int saved_ignore_embedded_pointers = pStubMsg->IgnoreEmbeddedPointers;
        pStubMsg->MemorySize = 0;
        pStubMsg->IgnoreEmbeddedPointers = 1;
        ret = m(pStubMsg, pFormat);
        pStubMsg->IgnoreEmbeddedPointers = saved_ignore_embedded_pointers;
        pStubMsg->Buffer = saved_buffer;
        return ret;
    }
    else
    {
        FIXME("format type 0x%x not implemented\n", pFormat[0]);
        RpcRaiseException(RPC_X_BAD_STUB_DATA);
        return 0;
    }
}

/* there can't be any alignment with the structures in this file */
#include "pshpack1.h"

#define STUBLESS_UNMARSHAL  1
#define STUBLESS_CALLSERVER 2
#define STUBLESS_CALCSIZE   3
#define STUBLESS_GETBUFFER  4
#define STUBLESS_MARSHAL    5
#define STUBLESS_FREE       6

/* From http://msdn.microsoft.com/library/default.asp?url=/library/en-us/rpc/rpc/parameter_descriptors.asp */
typedef struct _NDR_PROC_HEADER
{
    /* type of handle to use:
     * RPC_FC_BIND_EXPLICIT = 0 - Explicit handle.
     *   Handle is passed as a parameter to the function.
     *   Indicates that explicit handle information follows the header,
     *   which actually describes the handle.
     * RPC_FC_BIND_GENERIC = 31 - Implicit handle with custom binding routines
     *   (MIDL_STUB_DESC::IMPLICIT_HANDLE_INFO::pGenericBindingInfo)
     * RPC_FC_BIND_PRIMITIVE = 32 - Implicit handle using handle_t created by
     *   calling application
     * RPC_FC_AUTO_HANDLE = 33 - Automatic handle
     * RPC_FC_CALLBACK_HANDLE = 34 - undocmented
     */
    unsigned char handle_type;

    /* procedure flags:
     * Oi_FULL_PTR_USED = 0x01 - A full pointer can have the value NULL and can
     *   change during the call from NULL to non-NULL and supports aliasing
     *   and cycles. Indicates that the NdrFullPointerXlatInit function
     *   should be called.
     * Oi_RPCSS_ALLOC_USED = 0x02 - Use RpcSS allocate/free routines instead of
     *   normal allocate/free routines
     * Oi_OBJECT_PROC = 0x04 - Indicates a procedure that is part of an OLE
     *   interface, rather than a DCE RPC interface.
     * Oi_HAS_RPCFLAGS = 0x08 - Indicates that the rpc_flags element is 
     *   present in the header.
     * Oi_HAS_COMM_OR_FAULT = 0x20 - If Oi_OBJECT_PROC not present only then
     *   indicates that the procedure has the comm_status or fault_status
     *   MIDL attribute.
     * Oi_OBJ_USE_V2_INTERPRETER = 0x20 - If Oi_OBJECT_PROC present only
     *   then indicates that the format string is in -Oif or -Oicf format
     * Oi_USE_NEW_INIT_ROUTINES = 0x40 - Use NdrXInitializeNew instead of
     *   NdrXInitialize?
     */
    unsigned char Oi_flags;

    /* the zero-based index of the procedure */
    unsigned short proc_num;

    /* total size of all parameters on the stack, including any "this"
     * pointer and/or return value */
    unsigned short stack_size;
} NDR_PROC_HEADER;

/* same as above struct except additional element rpc_flags */
typedef struct _NDR_PROC_HEADER_RPC
{
    unsigned char handle_type;
    unsigned char Oi_flags;

    /*
     * RPCF_Idempotent = 0x0001 - [idempotent] MIDL attribute
     * RPCF_Broadcast = 0x0002 - [broadcast] MIDL attribute
     * RPCF_Maybe = 0x0004 - [maybe] MIDL attribute
     * Reserved = 0x0008 - 0x0080
     * RPCF_Message = 0x0100 - [message] MIDL attribute
     * Reserved = 0x0200 - 0x1000
     * RPCF_InputSynchronous = 0x2000 - unknown
     * RPCF_Asynchronous = 0x4000 - [async] MIDL attribute
     * Reserved = 0x8000
     */
    unsigned long rpc_flags;
    unsigned short proc_num;
    unsigned short stack_size;

} NDR_PROC_HEADER_RPC;

typedef struct _NDR_PROC_PARTIAL_OIF_HEADER
{
    /* the pre-computed client buffer size so that interpreter can skip all
     * or some (if the flag RPC_FC_PROC_OI2F_CLTMUSTSIZE is specified) of the
     * sizing pass */
    unsigned short constant_client_buffer_size;

    /* the pre-computed server buffer size so that interpreter can skip all
     * or some (if the flag RPC_FC_PROC_OI2F_SRVMUSTSIZE is specified) of the
     * sizing pass */
    unsigned short constant_server_buffer_size;

    INTERPRETER_OPT_FLAGS Oi2Flags;

    /* number of params */
    unsigned char number_of_params;
} NDR_PROC_PARTIAL_OIF_HEADER;

typedef struct _NDR_PARAM_OI_BASETYPE
{
    /* parameter direction. One of:
     * FC_IN_PARAM_BASETYPE = 0x4e - an in param
     * FC_RETURN_PARAM_BASETYPE = 0x53 - a return param
     */
    unsigned char param_direction;

    /* One of: FC_BYTE,FC_CHAR,FC_SMALL,FC_USMALL,FC_WCHAR,FC_SHORT,FC_USHORT,
     * FC_LONG,FC_ULONG,FC_FLOAT,FC_HYPER,FC_DOUBLE,FC_ENUM16,FC_ENUM32,
     * FC_ERROR_STATUS_T,FC_INT3264,FC_UINT3264 */
    unsigned char type_format_char;
} NDR_PARAM_OI_BASETYPE;

typedef struct _NDR_PARAM_OI_OTHER
{
    /* One of:
     * FC_IN_PARAM = 0x4d - An in param
     * FC_IN_OUT_PARAM = 0x50 - An in/out param
     * FC_OUT_PARAM = 0x51 - An out param
     * FC_RETURN_PARAM = 0x52 - A return value
     * FC_IN_PARAM_NO_FREE_INST = 0x4f - A param for which no freeing is done
     */
    unsigned char param_direction;

    /* Size of param on stack in NUMBERS OF INTS */
    unsigned char stack_size;

    /* offset in the type format string table */
    unsigned short type_offset;
} NDR_PARAM_OI_OTHER;

typedef struct _NDR_PARAM_OIF_BASETYPE
{
    PARAM_ATTRIBUTES param_attributes;

    /* the offset on the calling stack where the parameter is located */
    unsigned short stack_offset;

    /* see NDR_PARAM_OI_BASETYPE::type_format_char */
    unsigned char type_format_char;

    /* always FC_PAD */
    unsigned char unused;
} NDR_PARAM_OIF_BASETYPE;

typedef struct _NDR_PARAM_OIF_OTHER
{
    PARAM_ATTRIBUTES param_attributes;

    /* see NDR_PARAM_OIF_BASETYPE::stack_offset */
    unsigned short stack_offset;

    /* offset into the provided type format string where the type for this
     * parameter starts */
    unsigned short type_offset;
} NDR_PARAM_OIF_OTHER;

/* explicit handle description for FC_BIND_PRIMITIVE type */
typedef struct _NDR_EHD_PRIMITIVE
{
    /* FC_BIND_PRIMITIVE */
    unsigned char handle_type;

    /* is the handle passed in via a pointer? */
    unsigned char flag;

    /* offset from the beginning of the stack to the handle in bytes */
    unsigned short offset;
} NDR_EHD_PRIMITIVE;

/* explicit handle description for FC_BIND_GENERIC type */
typedef struct _NDR_EHD_GENERIC
{
    /* FC_BIND_GENERIC */
    unsigned char handle_type;

    /* upper 4bits is a flag indicating whether the handle is passed in
     * via a pointer. lower 4bits is the size of the user defined generic
     * handle type. the size must be less than or equal to the machine
     * register size */
    unsigned char flag_and_size;

    /* offset from the beginning of the stack to the handle in bytes */
    unsigned short offset;

    /* the index into the aGenericBindingRoutinesPairs field of MIDL_STUB_DESC
     * giving the bind and unbind routines for the handle */
    unsigned char binding_routine_pair_index;

    /* FC_PAD */
    unsigned char unused;
} NDR_EHD_GENERIC;

/* explicit handle description for FC_BIND_CONTEXT type */
typedef struct _NDR_EHD_CONTEXT
{
    /* FC_BIND_CONTEXT */
    unsigned char handle_type;

    /* Any of the following flags:
     * NDR_CONTEXT_HANDLE_CANNOT_BE_NULL = 0x01
     * NDR_CONTEXT_HANDLE_SERIALIZE = 0x02
     * NDR_CONTEXT_HANDLE_NO_SERIALIZE = 0x04
     * NDR_STRICT_CONTEXT_HANDLE = 0x08
     * HANDLE_PARAM_IS_OUT = 0x20
     * HANDLE_PARAM_IS_RETURN = 0x21
     * HANDLE_PARAM_IS_IN = 0x40
     * HANDLE_PARAM_IS_VIA_PTR = 0x80
     */
    unsigned char flags;

    /* offset from the beginning of the stack to the handle in bytes */
    unsigned short offset;

    /* zero-based index on rundown routine in apfnNdrRundownRoutines field
     * of MIDL_STUB_DESC */
    unsigned char context_rundown_routine_index;

    /* varies depending on NDR version used.
     * V1: zero-based index into parameters 
     * V2: zero-based index into handles that are parameters */
    unsigned char param_num;
} NDR_EHD_CONTEXT;

#include "poppack.h"

void WINAPI NdrRpcSmSetClientToOsf(PMIDL_STUB_MESSAGE pMessage)
{
#if 0 /* these functions are not defined yet */
    pMessage->pfnAllocate = NdrRpcSmClientAllocate;
    pMessage->pfnFree = NdrRpcSmClientFree;
#endif
}

static void WINAPI dump_RPC_FC_PROC_PF(PARAM_ATTRIBUTES param_attributes)
{
    if (param_attributes.MustSize) TRACE(" MustSize");
    if (param_attributes.MustFree) TRACE(" MustFree");
    if (param_attributes.IsPipe) TRACE(" IsPipe");
    if (param_attributes.IsIn) TRACE(" IsIn");
    if (param_attributes.IsOut) TRACE(" IsOut");
    if (param_attributes.IsReturn) TRACE(" IsReturn");
    if (param_attributes.IsBasetype) TRACE(" IsBasetype");
    if (param_attributes.IsByValue) TRACE(" IsByValue");
    if (param_attributes.IsSimpleRef) TRACE(" IsSimpleRef");
    if (param_attributes.IsDontCallFreeInst) TRACE(" IsDontCallFreeInst");
    if (param_attributes.SaveForAsyncFinish) TRACE(" SaveForAsyncFinish");
    if (param_attributes.ServerAllocSize) TRACE(" ServerAllocSize = %d", param_attributes.ServerAllocSize * 8);
}

static void WINAPI dump_INTERPRETER_OPT_FLAGS(INTERPRETER_OPT_FLAGS Oi2Flags)
{
    if (Oi2Flags.ServerMustSize) TRACE(" ServerMustSize");
    if (Oi2Flags.ClientMustSize) TRACE(" ClientMustSize");
    if (Oi2Flags.HasReturn) TRACE(" HasReturn");
    if (Oi2Flags.HasPipes) TRACE(" HasPipes");
    if (Oi2Flags.Unused) TRACE(" Unused");
    if (Oi2Flags.HasAsyncUuid) TRACE(" HasAsyncUuid");
    if (Oi2Flags.HasExtensions) TRACE(" HasExtensions");
    if (Oi2Flags.HasAsyncHandle) TRACE(" HasAsyncHandle");
    TRACE("\n");
}

#define ARG_FROM_OFFSET(stubMsg, offset) ((stubMsg).StackTop + (offset))

static PFORMAT_STRING client_get_handle(
    PMIDL_STUB_MESSAGE pStubMsg, const NDR_PROC_HEADER *pProcHeader,
    PFORMAT_STRING pFormat, handle_t *phBinding)
{
    /* binding */
    switch (pProcHeader->handle_type)
    {
    /* explicit binding: parse additional section */
    case RPC_FC_BIND_EXPLICIT:
        switch (*pFormat) /* handle_type */
        {
        case RPC_FC_BIND_PRIMITIVE: /* explicit primitive */
            {
                const NDR_EHD_PRIMITIVE *pDesc = (const NDR_EHD_PRIMITIVE *)pFormat;

                TRACE("Explicit primitive handle @ %d\n", pDesc->offset);

                if (pDesc->flag) /* pointer to binding */
                    *phBinding = **(handle_t **)ARG_FROM_OFFSET(*pStubMsg, pDesc->offset);
                else
                    *phBinding = *(handle_t *)ARG_FROM_OFFSET(*pStubMsg, pDesc->offset);
                return pFormat + sizeof(NDR_EHD_PRIMITIVE);
            }
        case RPC_FC_BIND_GENERIC: /* explicit generic */
            {
                const NDR_EHD_GENERIC *pDesc = (const NDR_EHD_GENERIC *)pFormat;
                void *pObject = NULL;
                void *pArg;
                const GENERIC_BINDING_ROUTINE_PAIR *pGenPair;

                TRACE("Explicit generic binding handle #%d\n", pDesc->binding_routine_pair_index);

                if (pDesc->flag_and_size & HANDLE_PARAM_IS_VIA_PTR)
                    pArg = *(void **)ARG_FROM_OFFSET(*pStubMsg, pDesc->offset);
                else
                    pArg = (void *)ARG_FROM_OFFSET(*pStubMsg, pDesc->offset);
                memcpy(&pObject, pArg, pDesc->flag_and_size & 0xf);
                pGenPair = &pStubMsg->StubDesc->aGenericBindingRoutinePairs[pDesc->binding_routine_pair_index];
                *phBinding = pGenPair->pfnBind(pObject);
                return pFormat + sizeof(NDR_EHD_GENERIC);
            }
        case RPC_FC_BIND_CONTEXT: /* explicit context */
            {
                const NDR_EHD_CONTEXT *pDesc = (const NDR_EHD_CONTEXT *)pFormat;
                NDR_CCONTEXT context_handle;
                TRACE("Explicit bind context\n");
                if (pDesc->flags & HANDLE_PARAM_IS_VIA_PTR)
                {
                    TRACE("\tHANDLE_PARAM_IS_VIA_PTR\n");
                    context_handle = **(NDR_CCONTEXT **)ARG_FROM_OFFSET(*pStubMsg, pDesc->offset);
                }
                else
                    context_handle = *(NDR_CCONTEXT *)ARG_FROM_OFFSET(*pStubMsg, pDesc->offset);
                if ((pDesc->flags & NDR_CONTEXT_HANDLE_CANNOT_BE_NULL) &&
                    !context_handle)
                {
                    ERR("null context handle isn't allowed\n");
                    RpcRaiseException(RPC_X_SS_IN_NULL_CONTEXT);
                    return NULL;
                }
                *phBinding = NDRCContextBinding(context_handle);
                /* FIXME: should we store this structure in stubMsg.pContext? */
                return pFormat + sizeof(NDR_EHD_CONTEXT);
            }
        default:
            ERR("bad explicit binding handle type (0x%02x)\n", pProcHeader->handle_type);
            RpcRaiseException(RPC_X_BAD_STUB_DATA);
        }
        break;
    case RPC_FC_BIND_GENERIC: /* implicit generic */
        FIXME("RPC_FC_BIND_GENERIC\n");
        RpcRaiseException(RPC_X_BAD_STUB_DATA); /* FIXME: remove when implemented */
        break;
    case RPC_FC_BIND_PRIMITIVE: /* implicit primitive */
        TRACE("Implicit primitive handle\n");
        *phBinding = *pStubMsg->StubDesc->IMPLICIT_HANDLE_INFO.pPrimitiveHandle;
        break;
    case RPC_FC_CALLBACK_HANDLE: /* implicit callback */
        FIXME("RPC_FC_CALLBACK_HANDLE\n");
        break;
    case RPC_FC_AUTO_HANDLE: /* implicit auto handle */
        /* strictly speaking, it isn't necessary to set hBinding here
         * since it isn't actually used (hence the automatic in its name),
         * but then why does MIDL generate a valid entry in the
         * MIDL_STUB_DESC for it? */
        TRACE("Implicit auto handle\n");
        *phBinding = *pStubMsg->StubDesc->IMPLICIT_HANDLE_INFO.pAutoHandle;
        break;
    default:
        ERR("bad implicit binding handle type (0x%02x)\n", pProcHeader->handle_type);
        RpcRaiseException(RPC_X_BAD_STUB_DATA);
    }
    return pFormat;
}

static void client_free_handle(
    PMIDL_STUB_MESSAGE pStubMsg, const NDR_PROC_HEADER *pProcHeader,
    PFORMAT_STRING pFormat, handle_t hBinding)
{
    /* binding */
    switch (pProcHeader->handle_type)
    {
    /* explicit binding: parse additional section */
    case RPC_FC_BIND_EXPLICIT:
        switch (*pFormat) /* handle_type */
        {
        case RPC_FC_BIND_GENERIC: /* explicit generic */
            {
                const NDR_EHD_GENERIC *pDesc = (const NDR_EHD_GENERIC *)pFormat;
                void *pObject = NULL;
                void *pArg;
                const GENERIC_BINDING_ROUTINE_PAIR *pGenPair;

                TRACE("Explicit generic binding handle #%d\n", pDesc->binding_routine_pair_index);

                if (pDesc->flag_and_size & HANDLE_PARAM_IS_VIA_PTR)
                    pArg = *(void **)ARG_FROM_OFFSET(*pStubMsg, pDesc->offset);
                else
                    pArg = (void *)ARG_FROM_OFFSET(*pStubMsg, pDesc->offset);
                memcpy(&pObject, pArg, pDesc->flag_and_size & 0xf);
                pGenPair = &pStubMsg->StubDesc->aGenericBindingRoutinePairs[pDesc->binding_routine_pair_index];
                pGenPair->pfnUnbind(pObject, hBinding);
                break;
            }
        case RPC_FC_BIND_CONTEXT: /* explicit context */
        case RPC_FC_BIND_PRIMITIVE: /* explicit primitive */
            break;
        default:
            ERR("bad explicit binding handle type (0x%02x)\n", pProcHeader->handle_type);
            RpcRaiseException(RPC_X_BAD_STUB_DATA);
        }
        break;
    case RPC_FC_BIND_GENERIC: /* implicit generic */
        FIXME("RPC_FC_BIND_GENERIC\n");
        RpcRaiseException(RPC_X_BAD_STUB_DATA); /* FIXME: remove when implemented */
        break;
    case RPC_FC_CALLBACK_HANDLE: /* implicit callback */
    case RPC_FC_BIND_PRIMITIVE: /* implicit primitive */
    case RPC_FC_AUTO_HANDLE: /* implicit auto handle */
        break;
    default:
        ERR("bad implicit binding handle type (0x%02x)\n", pProcHeader->handle_type);
        RpcRaiseException(RPC_X_BAD_STUB_DATA);
    }
}

static void client_do_args(PMIDL_STUB_MESSAGE pStubMsg, PFORMAT_STRING pFormat,
    int phase, unsigned short number_of_params, unsigned char *pRetVal)
{
    /* current format string offset */
    int current_offset = 0;
    /* current stack offset */
    unsigned short current_stack_offset = 0;
    /* counter */
    unsigned short i;

    for (i = 0; i < number_of_params; i++)
    {
        const NDR_PARAM_OIF_BASETYPE *pParam =
            (const NDR_PARAM_OIF_BASETYPE *)&pFormat[current_offset];
        unsigned char * pArg;

        current_stack_offset = pParam->stack_offset;
        pArg = ARG_FROM_OFFSET(*pStubMsg, current_stack_offset);

        TRACE("param[%d]: new format\n", i);
        TRACE("\tparam_attributes:"); dump_RPC_FC_PROC_PF(pParam->param_attributes); TRACE("\n");
        TRACE("\tstack_offset: 0x%x\n", current_stack_offset);
        TRACE("\tmemory addr (before): %p\n", pArg);

        if (pParam->param_attributes.IsBasetype)
        {
            const unsigned char * pTypeFormat =
                &pParam->type_format_char;

            if (pParam->param_attributes.IsSimpleRef)
                pArg = *(unsigned char **)pArg;

            TRACE("\tbase type: 0x%02x\n", *pTypeFormat);

            switch (phase)
            {
            case PROXY_CALCSIZE:
                if (pParam->param_attributes.IsIn)
                    call_buffer_sizer(pStubMsg, pArg, pTypeFormat);
                break;
            case PROXY_MARSHAL:
                if (pParam->param_attributes.IsIn)
                    call_marshaller(pStubMsg, pArg, pTypeFormat);
                break;
            case PROXY_UNMARSHAL:
                if (pParam->param_attributes.IsOut)
                {
                    if (pParam->param_attributes.IsReturn)
                        call_unmarshaller(pStubMsg, &pRetVal, pTypeFormat, 0);
                    else
                        call_unmarshaller(pStubMsg, &pArg, pTypeFormat, 0);
                    TRACE("pRetVal = %p\n", pRetVal);
                }
                break;
            default:
                RpcRaiseException(RPC_S_INTERNAL_ERROR);
            }

            current_offset += sizeof(NDR_PARAM_OIF_BASETYPE);
        }
        else
        {
            const NDR_PARAM_OIF_OTHER *pParamOther =
                (const NDR_PARAM_OIF_OTHER *)&pFormat[current_offset];

            const unsigned char * pTypeFormat =
                &(pStubMsg->StubDesc->pFormatTypes[pParamOther->type_offset]);

            /* if a simple ref pointer then we have to do the
             * check for the pointer being non-NULL. */
            if (pParam->param_attributes.IsSimpleRef)
            {
                if (!*(unsigned char **)pArg)
                    RpcRaiseException(RPC_X_NULL_REF_POINTER);
            }

            TRACE("\tcomplex type: 0x%02x\n", *pTypeFormat);

            switch (phase)
            {
            case PROXY_CALCSIZE:
                if (pParam->param_attributes.IsIn)
                {
                    if (pParam->param_attributes.IsByValue)
                        call_buffer_sizer(pStubMsg, pArg, pTypeFormat);
                    else
                        call_buffer_sizer(pStubMsg, *(unsigned char **)pArg, pTypeFormat);
                }
                break;
            case PROXY_MARSHAL:
                if (pParam->param_attributes.IsIn)
                {
                    if (pParam->param_attributes.IsByValue)
                        call_marshaller(pStubMsg, pArg, pTypeFormat);
                    else
                        call_marshaller(pStubMsg, *(unsigned char **)pArg, pTypeFormat);
                }
                break;
            case PROXY_UNMARSHAL:
                if (pParam->param_attributes.IsOut)
                {
                    if (pParam->param_attributes.IsReturn)
                        call_unmarshaller(pStubMsg, &pRetVal, pTypeFormat, 0);
                    else if (pParam->param_attributes.IsByValue)
                        call_unmarshaller(pStubMsg, &pArg, pTypeFormat, 0);
                    else
                        call_unmarshaller(pStubMsg, (unsigned char **)pArg, pTypeFormat, 0);
                }
                break;
            default:
                RpcRaiseException(RPC_S_INTERNAL_ERROR);
            }

            current_offset += sizeof(NDR_PARAM_OIF_OTHER);
        }
        TRACE("\tmemory addr (after): %p\n", pArg);
    }
}

static void client_do_args_old_format(PMIDL_STUB_MESSAGE pStubMsg,
    PFORMAT_STRING pFormat, int phase, unsigned short stack_size,
    unsigned char *pRetVal, BOOL object_proc)
{
    /* current format string offset */
    int current_offset = 0;
    /* current stack offset */
    unsigned short current_stack_offset = 0;
    /* counter */
    unsigned short i;

    /* NOTE: V1 style format does't terminate on the number_of_params
     * condition as it doesn't have this attribute. Instead it
     * terminates when the stack size given in the header is exceeded.
     */
    for (i = 0; TRUE; i++)
    {
        const NDR_PARAM_OI_BASETYPE *pParam =
            (const NDR_PARAM_OI_BASETYPE *)&pFormat[current_offset];
        /* note: current_stack_offset starts after the This pointer
         * if present, so adjust this */
        unsigned short current_stack_offset_adjusted = current_stack_offset +
            (object_proc ? sizeof(void *) : 0);
        unsigned char * pArg = ARG_FROM_OFFSET(*pStubMsg, current_stack_offset_adjusted);

        /* no more parameters; exit loop */
        if (current_stack_offset_adjusted >= stack_size)
            break;

        TRACE("param[%d]: old format\n", i);
        TRACE("\tparam_direction: 0x%x\n", pParam->param_direction);
        TRACE("\tstack_offset: 0x%x\n", current_stack_offset_adjusted);
        TRACE("\tmemory addr (before): %p\n", pArg);

        if (pParam->param_direction == RPC_FC_IN_PARAM_BASETYPE ||
            pParam->param_direction == RPC_FC_RETURN_PARAM_BASETYPE)
        {
            const unsigned char * pTypeFormat =
                &pParam->type_format_char;

            TRACE("\tbase type 0x%02x\n", *pTypeFormat);

            switch (phase)
            {
            case PROXY_CALCSIZE:
                if (pParam->param_direction == RPC_FC_IN_PARAM_BASETYPE)
                    call_buffer_sizer(pStubMsg, pArg, pTypeFormat);
                break;
            case PROXY_MARSHAL:
                if (pParam->param_direction == RPC_FC_IN_PARAM_BASETYPE)
                    call_marshaller(pStubMsg, pArg, pTypeFormat);
                break;
            case PROXY_UNMARSHAL:
                if (pParam->param_direction == RPC_FC_RETURN_PARAM_BASETYPE)
                {
                    if (pParam->param_direction & RPC_FC_RETURN_PARAM)
                        call_unmarshaller(pStubMsg, (unsigned char **)pRetVal, pTypeFormat, 0);
                    else
                        call_unmarshaller(pStubMsg, &pArg, pTypeFormat, 0);
                }
                break;
            default:
                RpcRaiseException(RPC_S_INTERNAL_ERROR);
            }

            current_stack_offset += call_memory_sizer(pStubMsg, pTypeFormat);
            current_offset += sizeof(NDR_PARAM_OI_BASETYPE);
        }
        else
        {
            const NDR_PARAM_OI_OTHER *pParamOther = 
                (const NDR_PARAM_OI_OTHER *)&pFormat[current_offset];

            const unsigned char *pTypeFormat =
                &pStubMsg->StubDesc->pFormatTypes[pParamOther->type_offset];

            TRACE("\tcomplex type 0x%02x\n", *pTypeFormat);

            switch (phase)
            {
            case PROXY_CALCSIZE:
                if (pParam->param_direction == RPC_FC_IN_PARAM ||
                    pParam->param_direction & RPC_FC_IN_OUT_PARAM)
                    call_buffer_sizer(pStubMsg, *(unsigned char **)pArg, pTypeFormat);
                break;
            case PROXY_MARSHAL:
                if (pParam->param_direction == RPC_FC_IN_PARAM ||
                    pParam->param_direction & RPC_FC_IN_OUT_PARAM)
                    call_marshaller(pStubMsg, *(unsigned char **)pArg, pTypeFormat);
                break;
            case PROXY_UNMARSHAL:
                if (pParam->param_direction == RPC_FC_IN_OUT_PARAM ||
                    pParam->param_direction == RPC_FC_OUT_PARAM)
                    call_unmarshaller(pStubMsg, (unsigned char **)pArg, pTypeFormat, 0);
                else if (pParam->param_direction == RPC_FC_RETURN_PARAM)
                    call_unmarshaller(pStubMsg, (unsigned char **)pRetVal, pTypeFormat, 0);
                break;
            default:
                RpcRaiseException(RPC_S_INTERNAL_ERROR);
            }

            current_stack_offset += pParamOther->stack_size * sizeof(INT);
            current_offset += sizeof(NDR_PARAM_OI_OTHER);
        }
        TRACE("\tmemory addr (after): %p\n", pArg);
    }
}

/* the return type should be CLIENT_CALL_RETURN, but this is incompatible
 * with the way gcc returns structures. "void *" should be the largest type
 * that MIDL should allow you to return anyway */
LONG_PTR WINAPIV NdrClientCall2(PMIDL_STUB_DESC pStubDesc, PFORMAT_STRING pFormat, ...)
{
    /* pointer to start of stack where arguments start */
    RPC_MESSAGE rpcMsg;
    MIDL_STUB_MESSAGE stubMsg;
    handle_t hBinding = NULL;
    /* procedure number */
    unsigned short procedure_number;
    /* size of stack */
    unsigned short stack_size;
    /* number of parameters. optional for client to give it to us */
    unsigned char number_of_params = ~0;
    /* cache of Oif_flags from v2 procedure header */
    INTERPRETER_OPT_FLAGS Oif_flags = { 0 };
    /* cache of extension flags from NDR_PROC_HEADER_EXTS */
    INTERPRETER_OPT_FLAGS2 ext_flags = { 0 };
    /* the type of pass we are currently doing */
    int phase;
    /* header for procedure string */
    const NDR_PROC_HEADER * pProcHeader = (const NDR_PROC_HEADER *)&pFormat[0];
    /* -Oif or -Oicf generated format */
    BOOL bV2Format = FALSE;
    /* the value to return to the client from the remote procedure */
    LONG_PTR RetVal = 0;
    /* the pointer to the object when in OLE mode */
    void * This = NULL;
    PFORMAT_STRING pHandleFormat;

    TRACE("pStubDesc %p, pFormat %p, ...\n", pStubDesc, pFormat);

    /* Later NDR language versions probably won't be backwards compatible */
    if (pStubDesc->Version > 0x50002)
    {
        FIXME("Incompatible stub description version: 0x%x\n", pStubDesc->Version);
        RpcRaiseException(RPC_X_WRONG_STUB_VERSION);
    }

    if (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_RPCFLAGS)
    {
        const NDR_PROC_HEADER_RPC *pProcHeader = (const NDR_PROC_HEADER_RPC *)&pFormat[0];
        stack_size = pProcHeader->stack_size;
        procedure_number = pProcHeader->proc_num;
        pFormat += sizeof(NDR_PROC_HEADER_RPC);
    }
    else
    {
        stack_size = pProcHeader->stack_size;
        procedure_number = pProcHeader->proc_num;
        pFormat += sizeof(NDR_PROC_HEADER);
    }
    TRACE("stack size: 0x%x\n", stack_size);
    TRACE("proc num: %d\n", procedure_number);

    /* create the full pointer translation tables, if requested */
    if (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_FULLPTR)
        stubMsg.FullPtrXlatTables = NdrFullPointerXlatInit(0,XLAT_CLIENT);

    if (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_OBJECT)
    {
        /* object is always the first argument */
        This = **(void *const **)(&pFormat+1);
        NdrProxyInitialize(This, &rpcMsg, &stubMsg, pStubDesc, procedure_number);
    }
    else
        NdrClientInitializeNew(&rpcMsg, &stubMsg, pStubDesc, procedure_number);

    TRACE("Oi_flags = 0x%02x\n", pProcHeader->Oi_flags);
    TRACE("MIDL stub version = 0x%x\n", pStubDesc->MIDLVersion);

    /* needed for conformance of top-level objects */
#ifdef __i386__
    stubMsg.StackTop = *(unsigned char **)(&pFormat+1);
#else
# warning Stack not retrieved for your CPU architecture
#endif

    pHandleFormat = pFormat;

    /* we only need a handle if this isn't an object method */
    if (!(pProcHeader->Oi_flags & RPC_FC_PROC_OIF_OBJECT))
    {
        pFormat = client_get_handle(&stubMsg, pProcHeader, pHandleFormat, &hBinding);
        if (!pFormat) return 0;
    }

    bV2Format = (pStubDesc->Version >= 0x20000);

    if (bV2Format)
    {
        const NDR_PROC_PARTIAL_OIF_HEADER *pOIFHeader =
            (const NDR_PROC_PARTIAL_OIF_HEADER *)pFormat;

        Oif_flags = pOIFHeader->Oi2Flags;
        number_of_params = pOIFHeader->number_of_params;

        pFormat += sizeof(NDR_PROC_PARTIAL_OIF_HEADER);
    }

    TRACE("Oif_flags = "); dump_INTERPRETER_OPT_FLAGS(Oif_flags);

    if (Oif_flags.HasExtensions)
    {
        const NDR_PROC_HEADER_EXTS *pExtensions =
            (const NDR_PROC_HEADER_EXTS *)pFormat;
        ext_flags = pExtensions->Flags2;
        pFormat += pExtensions->Size;
    }

    stubMsg.BufferLength = 0;

    /* store the RPC flags away */
    if (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_RPCFLAGS)
        rpcMsg.RpcFlags = ((const NDR_PROC_HEADER_RPC *)pProcHeader)->rpc_flags;

    /* use alternate memory allocation routines */
    if (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_RPCSSALLOC)
        NdrRpcSmSetClientToOsf(&stubMsg);

    if (Oif_flags.HasPipes)
    {
        FIXME("pipes not supported yet\n");
        RpcRaiseException(RPC_X_WRONG_STUB_VERSION); /* FIXME: remove when implemented */
        /* init pipes package */
        /* NdrPipesInitialize(...) */
    }
    if (ext_flags.HasNewCorrDesc)
    {
        /* initialize extra correlation package */
        FIXME("new correlation description not implemented\n");
        stubMsg.fHasNewCorrDesc = TRUE;
    }

    /* order of phases:
     * 1. PROXY_CALCSIZE - calculate the buffer size
     * 2. PROXY_GETBUFFER - allocate the buffer
     * 3. PROXY_MARHSAL - marshal [in] params into the buffer
     * 4. PROXY_SENDRECEIVE - send/receive buffer
     * 5. PROXY_UNMARHSAL - unmarshal [out] params from buffer
     */
    for (phase = PROXY_CALCSIZE; phase <= PROXY_UNMARSHAL; phase++)
    {
        TRACE("phase = %d\n", phase);
        switch (phase)
        {
        case PROXY_GETBUFFER:
            /* allocate the buffer */
            if (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_OBJECT)
                NdrProxyGetBuffer(This, &stubMsg);
            else if (Oif_flags.HasPipes)
                /* NdrGetPipeBuffer(...) */
                FIXME("pipes not supported yet\n");
            else
            {
                if (pProcHeader->handle_type == RPC_FC_AUTO_HANDLE)
#if 0
                    NdrNsGetBuffer(&stubMsg, stubMsg.BufferLength, hBinding);
#else
                    FIXME("using auto handle - call NdrNsGetBuffer when it gets implemented\n");
#endif
                else
                    NdrGetBuffer(&stubMsg, stubMsg.BufferLength, hBinding);
            }
            break;
        case PROXY_SENDRECEIVE:
            /* send the [in] params and receive the [out] and [retval]
             * params */
            if (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_OBJECT)
                NdrProxySendReceive(This, &stubMsg);
            else if (Oif_flags.HasPipes)
                /* NdrPipesSendReceive(...) */
                FIXME("pipes not supported yet\n");
            else
            {
                if (pProcHeader->handle_type == RPC_FC_AUTO_HANDLE)
#if 0
                    NdrNsSendReceive(&stubMsg, stubMsg.Buffer, pStubDesc->IMPLICIT_HANDLE_INFO.pAutoHandle);
#else
                    FIXME("using auto handle - call NdrNsSendReceive when it gets implemented\n");
#endif
                else
                    NdrSendReceive(&stubMsg, stubMsg.Buffer);
            }

            /* convert strings, floating point values and endianess into our
             * preferred format */
            if ((rpcMsg.DataRepresentation & 0x0000FFFFUL) != NDR_LOCAL_DATA_REPRESENTATION)
                NdrConvert(&stubMsg, pFormat);

            break;
        case PROXY_CALCSIZE:
        case PROXY_MARSHAL:
        case PROXY_UNMARSHAL:
            if (bV2Format)
                client_do_args(&stubMsg, pFormat, phase, number_of_params,
                    (unsigned char *)&RetVal);
            else
                client_do_args_old_format(&stubMsg, pFormat, phase, stack_size,
                    (unsigned char *)&RetVal,
                    (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_OBJECT));
            break;
        default:
            ERR("shouldn't reach here. phase %d\n", phase);
            break;
        }
    }

    if (ext_flags.HasNewCorrDesc)
    {
        /* free extra correlation package */
        /* NdrCorrelationFree(&stubMsg); */
    }

    if (Oif_flags.HasPipes)
    {
        /* NdrPipesDone(...) */
    }

    /* free the full pointer translation tables */
    if (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_FULLPTR)
        NdrFullPointerXlatFree(stubMsg.FullPtrXlatTables);

    /* free marshalling buffer */
    if (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_OBJECT)
        NdrProxyFreeBuffer(This, &stubMsg);
    else
    {
        NdrFreeBuffer(&stubMsg);
        client_free_handle(&stubMsg, pProcHeader, pHandleFormat, hBinding);
    }

    TRACE("RetVal = 0x%lx\n", RetVal);

    return RetVal;
}

/* calls a function with the specificed arguments, restoring the stack
 * properly afterwards as we don't know the calling convention of the
 * function */
#if defined __i386__ && defined _MSC_VER
__declspec(naked) LONG_PTR __cdecl call_server_func(SERVER_ROUTINE func, unsigned char * args, unsigned int stack_size)
{
    __asm
    {
        push ebp
        push edi            ; Save registers
        push esi
        mov ebp, esp
        mov eax, [ebp+16]   ; Get stack size
        sub esp, eax        ; Make room in stack for arguments
        mov edi, esp
        mov ecx, eax
        mov esi, [ebp+12]
        shr ecx, 2
        cld
        rep movsd           ; Copy dword blocks
        call [ebp+8]        ; Call function
        lea esp, [ebp-8]    ; Restore stack
        pop esi             ; Restore registers
        pop edi
        pop ebp
        ret
    }
}
#elif defined __i386__ && defined __GNUC__
LONG_PTR __cdecl call_server_func(SERVER_ROUTINE func, unsigned char * args, unsigned int stack_size);
__ASM_GLOBAL_FUNC(call_server_func,
    "pushl %ebp\n\t"
    "movl %esp, %ebp\n\t"
    "pushl %edi\n\t"            /* Save registers */
    "pushl %esi\n\t"
    "movl 16(%ebp), %eax\n\t"   /* Get stack size */
    "subl %eax, %esp\n\t"       /* Make room in stack for arguments */
    "andl $~15, %esp\n\t"	/* Make sure stack has 16-byte alignment for Mac OS X */
    "movl %esp, %edi\n\t"
    "movl %eax, %ecx\n\t"
    "movl 12(%ebp), %esi\n\t"
    "shrl $2, %ecx\n\t"         /* divide by 4 */
    "cld\n\t"
    "rep; movsl\n\t"            /* Copy dword blocks */
    "call *8(%ebp)\n\t"         /* Call function */
    "leal -8(%ebp), %esp\n\t"   /* Restore stack */
    "popl %esi\n\t"             /* Restore registers */
    "popl %edi\n\t"
    "popl %ebp\n\t"
    "ret\n" )
#else
#warning call_server_func not implemented for your architecture
LONG_PTR __cdecl call_server_func(SERVER_ROUTINE func, unsigned char * args, unsigned short stack_size)
{
    FIXME("Not implemented for your architecture\n");
    return 0;
}
#endif

static DWORD calc_arg_size(MIDL_STUB_MESSAGE *pStubMsg, PFORMAT_STRING pFormat)
{
    DWORD size;
    switch(*pFormat)
    {
    case RPC_FC_STRUCT:
        size = *(const WORD*)(pFormat + 2);
        break;
    case RPC_FC_CARRAY:
        size = *(const WORD*)(pFormat + 2);
        ComputeConformance(pStubMsg, NULL, pFormat + 4, 0);
        size *= pStubMsg->MaxCount;
        break;
    case RPC_FC_SMFARRAY:
        size = *(const WORD*)(pFormat + 2);
        break;
    case RPC_FC_LGFARRAY:
        size = *(const DWORD*)(pFormat + 2);
        break;
    default:
        FIXME("Unhandled type %02x\n", *pFormat);
        /* fallthrough */
    case RPC_FC_RP:
        size = sizeof(void *);
        break;
    }
    return size;
}

/* FIXME: need to free some stuff in here too */
LONG WINAPI NdrStubCall2(
    struct IRpcStubBuffer * pThis,
    struct IRpcChannelBuffer * pChannel,
    PRPC_MESSAGE pRpcMsg,
    DWORD * pdwStubPhase)
{
    const MIDL_SERVER_INFO *pServerInfo;
    const MIDL_STUB_DESC *pStubDesc;
    PFORMAT_STRING pFormat;
    MIDL_STUB_MESSAGE stubMsg;
    /* pointer to start of stack to pass into stub implementation */
    unsigned char * args;
    /* size of stack */
    unsigned short stack_size;
    /* current stack offset */
    unsigned short current_stack_offset;
    /* number of parameters. optional for client to give it to us */
    unsigned char number_of_params = ~0;
    /* counter */
    unsigned short i;
    /* cache of Oif_flags from v2 procedure header */
    INTERPRETER_OPT_FLAGS Oif_flags = { 0 };
    /* cache of extension flags from NDR_PROC_HEADER_EXTS */
    INTERPRETER_OPT_FLAGS2 ext_flags = { 0 };
    /* the type of pass we are currently doing */
    int phase;
    /* header for procedure string */
    const NDR_PROC_HEADER *pProcHeader;
    /* offset in format string for start of params */
    int parameter_start_offset;
    /* current format string offset */
    int current_offset;
    /* -Oif or -Oicf generated format */
    BOOL bV2Format = FALSE;
    /* location to put retval into */
    LONG_PTR *retval_ptr = NULL;

    TRACE("pThis %p, pChannel %p, pRpcMsg %p, pdwStubPhase %p\n", pThis, pChannel, pRpcMsg, pdwStubPhase);

    if (pThis)
        pServerInfo = CStdStubBuffer_GetServerInfo(pThis);
    else
        pServerInfo = ((RPC_SERVER_INTERFACE *)pRpcMsg->RpcInterfaceInformation)->InterpreterInfo;

    pStubDesc = pServerInfo->pStubDesc;
    pFormat = pServerInfo->ProcString + pServerInfo->FmtStringOffset[pRpcMsg->ProcNum];
    pProcHeader = (const NDR_PROC_HEADER *)&pFormat[0];

    /* Later NDR language versions probably won't be backwards compatible */
    if (pStubDesc->Version > 0x50002)
    {
        FIXME("Incompatible stub description version: 0x%x\n", pStubDesc->Version);
        RpcRaiseException(RPC_X_WRONG_STUB_VERSION);
    }

    /* create the full pointer translation tables, if requested */
    if (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_FULLPTR)
        stubMsg.FullPtrXlatTables = NdrFullPointerXlatInit(0,XLAT_SERVER);

    if (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_RPCFLAGS)
    {
        const NDR_PROC_HEADER_RPC *pProcHeader = (const NDR_PROC_HEADER_RPC *)&pFormat[0];
        stack_size = pProcHeader->stack_size;
        current_offset = sizeof(NDR_PROC_HEADER_RPC);

    }
    else
    {
        stack_size = pProcHeader->stack_size;
        current_offset = sizeof(NDR_PROC_HEADER);
    }

    TRACE("Oi_flags = 0x%02x\n", pProcHeader->Oi_flags);

    /* binding */
    switch (pProcHeader->handle_type)
    {
    /* explicit binding: parse additional section */
    case RPC_FC_BIND_EXPLICIT:
        switch (pFormat[current_offset]) /* handle_type */
        {
        case RPC_FC_BIND_PRIMITIVE: /* explicit primitive */
            current_offset += sizeof(NDR_EHD_PRIMITIVE);
            break;
        case RPC_FC_BIND_GENERIC: /* explicit generic */
            current_offset += sizeof(NDR_EHD_GENERIC);
            break;
        case RPC_FC_BIND_CONTEXT: /* explicit context */
            current_offset += sizeof(NDR_EHD_CONTEXT);
            break;
        default:
            ERR("bad explicit binding handle type (0x%02x)\n", pProcHeader->handle_type);
            RpcRaiseException(RPC_X_BAD_STUB_DATA);
        }
        break;
    case RPC_FC_BIND_GENERIC: /* implicit generic */
    case RPC_FC_BIND_PRIMITIVE: /* implicit primitive */
    case RPC_FC_CALLBACK_HANDLE: /* implicit callback */
    case RPC_FC_AUTO_HANDLE: /* implicit auto handle */
        break;
    default:
        ERR("bad implicit binding handle type (0x%02x)\n", pProcHeader->handle_type);
        RpcRaiseException(RPC_X_BAD_STUB_DATA);
    }

    bV2Format = (pStubDesc->Version >= 0x20000);

    if (bV2Format)
    {
        const NDR_PROC_PARTIAL_OIF_HEADER *pOIFHeader =
            (const NDR_PROC_PARTIAL_OIF_HEADER *)&pFormat[current_offset];

        Oif_flags = pOIFHeader->Oi2Flags;
        number_of_params = pOIFHeader->number_of_params;

        current_offset += sizeof(NDR_PROC_PARTIAL_OIF_HEADER);
    }

    TRACE("Oif_flags = "); dump_INTERPRETER_OPT_FLAGS(Oif_flags);

    if (Oif_flags.HasExtensions)
    {
        const NDR_PROC_HEADER_EXTS *pExtensions =
            (const NDR_PROC_HEADER_EXTS *)&pFormat[current_offset];
        ext_flags = pExtensions->Flags2;
        current_offset += pExtensions->Size;
    }

    if (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_OBJECT)
        NdrStubInitialize(pRpcMsg, &stubMsg, pStubDesc, pChannel);
    else
        NdrServerInitializeNew(pRpcMsg, &stubMsg, pStubDesc);

    /* store the RPC flags away */
    if (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_RPCFLAGS)
        pRpcMsg->RpcFlags = ((const NDR_PROC_HEADER_RPC *)pProcHeader)->rpc_flags;

    /* use alternate memory allocation routines */
    if (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_RPCSSALLOC)
#if 0
          NdrRpcSsEnableAllocate(&stubMsg);
#else
          FIXME("Set RPCSS memory allocation routines\n");
#endif

    if (Oif_flags.HasPipes)
    {
        FIXME("pipes not supported yet\n");
        RpcRaiseException(RPC_X_WRONG_STUB_VERSION); /* FIXME: remove when implemented */
        /* init pipes package */
        /* NdrPipesInitialize(...) */
    }
    if (ext_flags.HasNewCorrDesc)
    {
        /* initialize extra correlation package */
        FIXME("new correlation description not implemented\n");
        stubMsg.fHasNewCorrDesc = TRUE;
    }

    /* convert strings, floating point values and endianess into our
     * preferred format */
    if ((pRpcMsg->DataRepresentation & 0x0000FFFFUL) != NDR_LOCAL_DATA_REPRESENTATION)
        NdrConvert(&stubMsg, pFormat);

    parameter_start_offset = current_offset;

    TRACE("allocating memory for stack of size %x\n", stack_size);

    args = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, stack_size);
    stubMsg.StackTop = args; /* used by conformance of top-level objects */

    /* add the implicit This pointer as the first arg to the function if we
     * are calling an object method */
    if (pThis)
        *(void **)args = ((CStdStubBuffer *)pThis)->pvServerObject;

    /* order of phases:
     * 1. STUBLESS_UNMARHSAL - unmarshal [in] params from buffer
     * 2. STUBLESS_CALLSERVER - send/receive buffer
     * 3. STUBLESS_CALCSIZE - get [out] buffer size
     * 4. STUBLESS_GETBUFFER - allocate [out] buffer
     * 5. STUBLESS_MARHSAL - marshal [out] params to buffer
     */
    for (phase = STUBLESS_UNMARSHAL; phase <= STUBLESS_FREE; phase++)
    {
        TRACE("phase = %d\n", phase);
        switch (phase)
        {
        case STUBLESS_CALLSERVER:
            /* call the server function */
            if (pServerInfo->ThunkTable && pServerInfo->ThunkTable[pRpcMsg->ProcNum])
                pServerInfo->ThunkTable[pRpcMsg->ProcNum](&stubMsg);
            else
            {
                SERVER_ROUTINE func;
                LONG_PTR retval;

                if (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_OBJECT)
                {
                    SERVER_ROUTINE *vtbl = *(SERVER_ROUTINE **)((CStdStubBuffer *)pThis)->pvServerObject;
                    func = vtbl[pRpcMsg->ProcNum];
                }
                else
                    func = pServerInfo->DispatchTable[pRpcMsg->ProcNum];

                /* FIXME: what happens with return values that don't fit into a single register on x86? */
                retval = call_server_func(func, args, stack_size);

                if (retval_ptr)
                {
                    TRACE("stub implementation returned 0x%lx\n", retval);
                    *retval_ptr = retval;
                }
                else
                    TRACE("void stub implementation\n");
            }

            stubMsg.Buffer = NULL;
            stubMsg.BufferLength = 0;

            break;
        case STUBLESS_GETBUFFER:
            if (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_OBJECT)
                NdrStubGetBuffer(pThis, pChannel, &stubMsg);
            else
            {
                RPC_STATUS Status;

                pRpcMsg->BufferLength = stubMsg.BufferLength;
                /* allocate buffer for [out] and [ret] params */
                Status = I_RpcGetBuffer(pRpcMsg); 
                if (Status)
                    RpcRaiseException(Status);
                stubMsg.BufferStart = pRpcMsg->Buffer;
                stubMsg.BufferEnd = stubMsg.BufferStart + stubMsg.BufferLength;
                stubMsg.Buffer = stubMsg.BufferStart;
            }
            break;
        case STUBLESS_MARSHAL:
        case STUBLESS_UNMARSHAL:
        case STUBLESS_CALCSIZE:
        case STUBLESS_FREE:
            current_offset = parameter_start_offset;
            current_stack_offset = 0;

            /* NOTE: V1 style format does't terminate on the number_of_params
             * condition as it doesn't have this attribute. Instead it
             * terminates when the stack size given in the header is exceeded.
             */
            for (i = 0; i < number_of_params; i++)
            {
                if (bV2Format) /* new parameter format */
                {
                    const NDR_PARAM_OIF_BASETYPE *pParam =
                        (const NDR_PARAM_OIF_BASETYPE *)&pFormat[current_offset];
                    unsigned char *pArg;

                    current_stack_offset = pParam->stack_offset;
                    pArg = (unsigned char *)(args+current_stack_offset);

                    TRACE("param[%d]: new format\n", i);
                    TRACE("\tparam_attributes:"); dump_RPC_FC_PROC_PF(pParam->param_attributes); TRACE("\n");
                    TRACE("\tstack_offset: 0x%x\n", current_stack_offset);
                    TRACE("\tmemory addr (before): %p -> %p\n", pArg, *(unsigned char **)pArg);

                    if (pParam->param_attributes.IsBasetype)
                    {
                        const unsigned char *pTypeFormat =
                            &pParam->type_format_char;

                        TRACE("\tbase type: 0x%02x\n", *pTypeFormat);

                        switch (phase)
                        {
                        case STUBLESS_MARSHAL:
                            if (pParam->param_attributes.IsOut || pParam->param_attributes.IsReturn)
                            {
                                if (pParam->param_attributes.IsSimpleRef)
                                    call_marshaller(&stubMsg, *(unsigned char **)pArg, pTypeFormat);
                                else
                                    call_marshaller(&stubMsg, pArg, pTypeFormat);
                            }
                            break;
                        case STUBLESS_FREE:
                            if (pParam->param_attributes.ServerAllocSize)
                                HeapFree(GetProcessHeap(), 0, *(void **)pArg);
                            break;
                        case STUBLESS_UNMARSHAL:
                            if (pParam->param_attributes.ServerAllocSize)
                                *(void **)pArg = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    pParam->param_attributes.ServerAllocSize * 8);

                            if (pParam->param_attributes.IsIn)
                            {
                                if (pParam->param_attributes.IsSimpleRef)
                                    call_unmarshaller(&stubMsg, (unsigned char **)pArg, pTypeFormat, 0);
                                else
                                    call_unmarshaller(&stubMsg, &pArg, pTypeFormat, 0);
                            }

                            /* make a note of the address of the return value parameter for later */
                            if (pParam->param_attributes.IsReturn)
                                retval_ptr = (LONG_PTR *)pArg;

                            break;
                        case STUBLESS_CALCSIZE:
                            if (pParam->param_attributes.IsOut || pParam->param_attributes.IsReturn)
                            {
                                if (pParam->param_attributes.IsSimpleRef)
                                    call_buffer_sizer(&stubMsg, *(unsigned char **)pArg, pTypeFormat);
                                else
                                    call_buffer_sizer(&stubMsg, pArg, pTypeFormat);
                            }
                            break;
                        default:
                            RpcRaiseException(RPC_S_INTERNAL_ERROR);
                        }

                        current_offset += sizeof(NDR_PARAM_OIF_BASETYPE);
                    }
                    else
                    {
                        const NDR_PARAM_OIF_OTHER *pParamOther =
                            (const NDR_PARAM_OIF_OTHER *)&pFormat[current_offset];

                        const unsigned char * pTypeFormat =
                            &(pStubDesc->pFormatTypes[pParamOther->type_offset]);

                        TRACE("\tcomplex type 0x%02x\n", *pTypeFormat);

                        switch (phase)
                        {
                        case STUBLESS_MARSHAL:
                            if (pParam->param_attributes.IsOut || pParam->param_attributes.IsReturn)
                            {
                                if (pParam->param_attributes.IsByValue)
                                    call_marshaller(&stubMsg, pArg, pTypeFormat);
                                else
                                    call_marshaller(&stubMsg, *(unsigned char **)pArg, pTypeFormat);
                            }
                            break;
                        case STUBLESS_FREE:
                            if (pParam->param_attributes.MustFree)
                            {
                                if (pParam->param_attributes.IsByValue)
                                    call_freer(&stubMsg, pArg, pTypeFormat);
                                else
                                    call_freer(&stubMsg, *(unsigned char **)pArg, pTypeFormat);
                            }

                            if (pParam->param_attributes.IsOut &&
                                !pParam->param_attributes.IsIn &&
                                !pParam->param_attributes.IsByValue &&
                                !pParam->param_attributes.ServerAllocSize)
                            {
                                stubMsg.pfnFree(*(void **)pArg);
                            }

                            if (pParam->param_attributes.ServerAllocSize)
                                HeapFree(GetProcessHeap(), 0, *(void **)pArg);
                            /* FIXME: call call_freer here for IN types with MustFree set */
                            break;
                        case STUBLESS_UNMARSHAL:
                            if (pParam->param_attributes.ServerAllocSize)
                                *(void **)pArg = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    pParam->param_attributes.ServerAllocSize * 8);

                            if (pParam->param_attributes.IsIn)
                            {
                                if (pParam->param_attributes.IsByValue)
                                    call_unmarshaller(&stubMsg, &pArg, pTypeFormat, 0);
                                else
                                    call_unmarshaller(&stubMsg, (unsigned char **)pArg, pTypeFormat, 0);
                            }
                            else if (pParam->param_attributes.IsOut &&
                                     !pParam->param_attributes.ServerAllocSize &&
                                     !pParam->param_attributes.IsByValue)
                            {
                                DWORD size = calc_arg_size(&stubMsg, pTypeFormat);

                                if(size)
                                {
                                    *(void **)pArg = NdrAllocate(&stubMsg, size);
                                    memset(*(void **)pArg, 0, size);
                                }
                            }
                            break;
                        case STUBLESS_CALCSIZE:
                            if (pParam->param_attributes.IsOut || pParam->param_attributes.IsReturn)
                            {
                                if (pParam->param_attributes.IsByValue)
                                    call_buffer_sizer(&stubMsg, pArg, pTypeFormat);
                                else
                                    call_buffer_sizer(&stubMsg, *(unsigned char **)pArg, pTypeFormat);
                            }
                            break;
                        default:
                            RpcRaiseException(RPC_S_INTERNAL_ERROR);
                        }

                        current_offset += sizeof(NDR_PARAM_OIF_OTHER);
                    }
                    TRACE("\tmemory addr (after): %p -> %p\n", pArg, *(unsigned char **)pArg);
                }
                else /* old parameter format */
                {
                    const NDR_PARAM_OI_BASETYPE *pParam =
                        (const NDR_PARAM_OI_BASETYPE *)&pFormat[current_offset];
                    /* note: current_stack_offset starts after the This pointer
                     * if present, so adjust this */
                    unsigned short current_stack_offset_adjusted = current_stack_offset +
                        ((pProcHeader->Oi_flags & RPC_FC_PROC_OIF_OBJECT) ? sizeof(void *) : 0);
                    unsigned char *pArg = (unsigned char *)(args+current_stack_offset_adjusted);

                    /* no more parameters; exit loop */
                    if (current_stack_offset_adjusted >= stack_size)
                        break;

                    TRACE("param[%d]: old format\n", i);
                    TRACE("\tparam_direction: 0x%x\n", pParam->param_direction);
                    TRACE("\tstack_offset: 0x%x\n", current_stack_offset_adjusted);

                    if (pParam->param_direction == RPC_FC_IN_PARAM_BASETYPE ||
                        pParam->param_direction == RPC_FC_RETURN_PARAM_BASETYPE)
                    {
                        const unsigned char *pTypeFormat =
                            &pParam->type_format_char;

                        TRACE("\tbase type 0x%02x\n", *pTypeFormat);

                        switch (phase)
                        {
                        case STUBLESS_MARSHAL:
                            if (pParam->param_direction == RPC_FC_RETURN_PARAM_BASETYPE)
                                call_marshaller(&stubMsg, pArg, pTypeFormat);
                            break;
                        case STUBLESS_FREE:
                            if (pParam->param_direction == RPC_FC_IN_PARAM_BASETYPE)
                                call_freer(&stubMsg, pArg, pTypeFormat);
                            break;
                        case STUBLESS_UNMARSHAL:
                            if (pParam->param_direction == RPC_FC_IN_PARAM_BASETYPE)
                                call_unmarshaller(&stubMsg, &pArg, pTypeFormat, 0);
                            else if (pParam->param_direction == RPC_FC_RETURN_PARAM_BASETYPE)
                                retval_ptr = (LONG_PTR *)pArg;
                            break;
                        case STUBLESS_CALCSIZE:
                            if (pParam->param_direction == RPC_FC_RETURN_PARAM_BASETYPE)
                                call_buffer_sizer(&stubMsg, pArg, pTypeFormat);
                            break;
                        default:
                            RpcRaiseException(RPC_S_INTERNAL_ERROR);
                        }

                        current_stack_offset += call_memory_sizer(&stubMsg, pTypeFormat);
                        current_offset += sizeof(NDR_PARAM_OI_BASETYPE);
                    }
                    else
                    {
                        const NDR_PARAM_OI_OTHER *pParamOther = 
                            (const NDR_PARAM_OI_OTHER *)&pFormat[current_offset];

                        const unsigned char * pTypeFormat =
                            &pStubDesc->pFormatTypes[pParamOther->type_offset];

                        TRACE("\tcomplex type 0x%02x\n", *pTypeFormat);

                        switch (phase)
                        {
                        case STUBLESS_MARSHAL:
                            if (pParam->param_direction == RPC_FC_OUT_PARAM ||
                                pParam->param_direction == RPC_FC_IN_OUT_PARAM ||
                                pParam->param_direction == RPC_FC_RETURN_PARAM)
                                call_marshaller(&stubMsg, *(unsigned char **)pArg, pTypeFormat);
                            break;
                        case STUBLESS_FREE:
                            if (pParam->param_direction == RPC_FC_IN_OUT_PARAM ||
                                pParam->param_direction == RPC_FC_IN_PARAM)
                                call_freer(&stubMsg, *(unsigned char **)pArg, pTypeFormat);
                            else if (pParam->param_direction == RPC_FC_OUT_PARAM)
                                stubMsg.pfnFree(*(void **)pArg);
                            break;
                        case STUBLESS_UNMARSHAL:
                            if (pParam->param_direction == RPC_FC_IN_OUT_PARAM ||
                                pParam->param_direction == RPC_FC_IN_PARAM)
                                call_unmarshaller(&stubMsg, (unsigned char **)pArg, pTypeFormat, 0);
                            else if (pParam->param_direction == RPC_FC_RETURN_PARAM)
                                retval_ptr = (LONG_PTR *)pArg;
                            else if (pParam->param_direction == RPC_FC_OUT_PARAM)
                            {
                                DWORD size = calc_arg_size(&stubMsg, pTypeFormat);

                                if(size)
                                {
                                    *(void **)pArg = NdrAllocate(&stubMsg, size);
                                    memset(*(void **)pArg, 0, size);
                                }
                            }
                            break;
                        case STUBLESS_CALCSIZE:
                            if (pParam->param_direction == RPC_FC_OUT_PARAM ||
                                pParam->param_direction == RPC_FC_IN_OUT_PARAM ||
                                pParam->param_direction == RPC_FC_RETURN_PARAM)
                                call_buffer_sizer(&stubMsg, *(unsigned char **)pArg, pTypeFormat);
                            break;
                        default:
                            RpcRaiseException(RPC_S_INTERNAL_ERROR);
                        }

                        current_stack_offset += pParamOther->stack_size * sizeof(INT);
                        current_offset += sizeof(NDR_PARAM_OI_OTHER);
                    }
                }
            }

            break;
        default:
            ERR("shouldn't reach here. phase %d\n", phase);
            break;
        }
    }

    pRpcMsg->BufferLength = (unsigned int)(stubMsg.Buffer - (unsigned char *)pRpcMsg->Buffer);

    if (ext_flags.HasNewCorrDesc)
    {
        /* free extra correlation package */
        /* NdrCorrelationFree(&stubMsg); */
    }

    if (Oif_flags.HasPipes)
    {
        /* NdrPipesDone(...) */
    }

    /* free the full pointer translation tables */
    if (pProcHeader->Oi_flags & RPC_FC_PROC_OIF_FULLPTR)
        NdrFullPointerXlatFree(stubMsg.FullPtrXlatTables);

    /* free server function stack */
    HeapFree(GetProcessHeap(), 0, args);

    return S_OK;
}

void WINAPI NdrServerCall2(PRPC_MESSAGE pRpcMsg)
{
    DWORD dwPhase;
    NdrStubCall2(NULL, NULL, pRpcMsg, &dwPhase);
}
