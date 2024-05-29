//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <xdpapi.h>
#include <afxdp_helper.h>

#pragma comment(lib, "xdpapi.lib")

extern void JoinGroupOnAllInterfaces(const char* group_address = "224.0.0.200");


const CHAR* UsageText =
    "xskfwd.exe <IfIndex>"
    "\n"
    "Forwards RX traffic using an XDP program and AF_XDP sockets. This sample\n"
    "application forwards traffic on the specified IfIndex originally destined to\n"
    "UDP port 1234 back to the sender. Only the 0th data path queue on the interface\n"
    "is used.\n";

const XDP_HOOK_ID XdpInspectRxL2 = {
    .Layer = XDP_HOOK_L2,
    .Direction = XDP_HOOK_RX,
    .SubLayer = XDP_HOOK_INSPECT,
};

#define LOGERR(...)               \
    fprintf(stderr, "ERR: ");     \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n")

static UINT16 htons(UINT16 value)
{
    UINT16 result = ((value & 0xff00) >> 8) | ((value & 0xff) << 8);
    //LOGERR("htons: %04x -> %04x", value, result);
    return result;
}

static void TranslateRxToTx(_Inout_ UCHAR* Frame, _In_ UINT32 Length)
{
    // Ethernet 0,  14
    // Ipv4:    14, 20
    // UDP      34, 8
    if (Length > 42) {
        UINT16 srcPort;
        UINT16 dstPort;
        memcpy(&srcPort, &Frame[34], 2); 
        memcpy(&dstPort, &Frame[36], 2); 
        LOGERR(
            "Length: %u: SrcPort: %04x, DstPort: %04x",
            Length, htons(srcPort), htons(dstPort));
    }

    memset(Frame, 0, Length);
}

int main(int argc, char** argv)
{
    const XDP_API_TABLE* XdpApi;
    HANDLE Socket;
    HANDLE Program;
    UINT32 IfIndex;
    XSK_RING_INFO_SET RingInfo;
    UINT32 OptionLength;
    XSK_RING RxRing;
    XSK_RING RxFillRing;
    UINT32 RingIndex;

    if (argc < 2) {
        fprintf(stderr, UsageText);
        return EXIT_FAILURE;
    }

    IfIndex = atoi(argv[1]);

    //
    // Retrieve the XDP API dispatch table.
    //
    if (auto Result = XdpOpenApi(XDP_API_VERSION_1, &XdpApi); FAILED(Result)) {
        LOGERR("XdpOpenApi failed: %x", Result);
        return EXIT_FAILURE;
    }

    //
    // Create an AF_XDP socket. The newly created socket is not connected.
    //
    if (auto Result = XdpApi->XskCreate(&Socket); FAILED(Result)) {
        LOGERR("XskCreate failed: %x", Result);
        return EXIT_FAILURE;
    }

    //
    // Register our frame buffer(s) with the AF_XDP socket. For simplicity, we
    // register a buffer containing a single frame. The registered buffer is
    // available mapped into AF_XDP's address space, and elements of descriptor
    // rings refer to relative offets from the start of the UMEM.
    //
    DWORD FrameSize = 16 * 1024;
    LPVOID Frame = VirtualAlloc(NULL, FrameSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (Frame == nullptr) {
        LOGERR("VirtualAlloc failed!");
        return EXIT_FAILURE;
    }

    XSK_UMEM_REG UmemReg {
        .TotalSize = FrameSize,
        .ChunkSize = FrameSize,
        .Address = Frame,
    };
    if (auto Result = XdpApi->XskSetSockopt(Socket, XSK_SOCKOPT_UMEM_REG, &UmemReg, sizeof(UmemReg)); FAILED(Result)) {
        LOGERR("XSK_UMEM_REG failed: %x", Result);
        return EXIT_FAILURE;
    }

    //
    // Bind the AF_XDP socket to the specified interface and 0th data path
    // queue, and indicate the intent to perform RX and TX actions.
    //
    if (auto Result = XdpApi->XskBind(Socket, IfIndex, 0, XSK_BIND_FLAG_RX); FAILED(Result)) {
        LOGERR("XskBind failed: %x", Result);
        return EXIT_FAILURE;
    }

    //
    // Request a set of RX, RX fill, TX, and TX completion descriptor rings.
    // Request a capacity of one frame in each ring for simplicity. XDP will
    // create the rings and map them into the process address space as part of
    // the XskActivate step further below.
    //

    UINT32 RingSize = 1;
    if (auto Result = XdpApi->XskSetSockopt(Socket, XSK_SOCKOPT_RX_RING_SIZE, &RingSize, sizeof(RingSize));
        FAILED(Result)) {
        LOGERR("XSK_SOCKOPT_RX_RING_SIZE failed: %x", Result);
        return EXIT_FAILURE;
    }

    if (auto Result = XdpApi->XskSetSockopt(Socket, XSK_SOCKOPT_RX_FILL_RING_SIZE, &RingSize, sizeof(RingSize));
        FAILED(Result)) {
        LOGERR("XSK_SOCKOPT_RX_FILL_RING_SIZE failed: %x", Result);
        return EXIT_FAILURE;
    }

    //
    // Activate the AF_XDP socket. Once activated, descriptor rings are
    // available and RX and TX can occur.
    //
    if (auto Result = XdpApi->XskActivate(Socket, XSK_ACTIVATE_FLAG_NONE); FAILED(Result)) {
        LOGERR("XskActivate failed: %x", Result);
        return EXIT_FAILURE;
    }

    //
    // Retrieve the RX, RX fill, TX, and TX completion ring info from AF_XDP.
    //
    OptionLength = sizeof(RingInfo);
    if (auto Result = XdpApi->XskGetSockopt(Socket, XSK_SOCKOPT_RING_INFO, &RingInfo, &OptionLength); FAILED(Result)) {
        LOGERR("XSK_SOCKOPT_RING_INFO failed: %x", Result);
        return EXIT_FAILURE;
    }

    //
    // Initialize the optional AF_XDP helper library with the socket ring info.
    // These helpers simplify manipulation of the shared rings.
    //
    XskRingInitialize(&RxRing, &RingInfo.Rx);
    XskRingInitialize(&RxFillRing, &RingInfo.Fill);

    //
    // Place an empty frame descriptor into the RX fill ring. When the AF_XDP
    // socket receives a frame from XDP, it will pop the first available
    // frame descriptor from the RX fill ring and copy the frame payload into
    // that descriptor's buffer.
    //
    XskRingProducerReserve(&RxFillRing, 1, &RingIndex);

    //
    // The value of each RX fill and TX completion ring element is an offset
    // from the start of the UMEM to the start of the frame. Since this sample
    // is using a single buffer, the offset is always zero.
    //
    *(UINT32*)XskRingGetElement(&RxFillRing, RingIndex) = 0;

    XskRingProducerSubmit(&RxFillRing, 1);

    //
    // Create an XDP program using the parsed rule at the L2 inspect hook point.
    // The rule intercepts all UDP frames destined to local port 1234 and
    // redirects them to the AF_XDP socket.
    //
    XDP_RULE Rule
    {
        .Match = XDP_MATCH_UDP,
        .Pattern = {
            .Port = htons(0x4322),
        },
        .Action = XDP_PROGRAM_ACTION_REDIRECT,
        .Redirect = {
            .TargetType = XDP_REDIRECT_TARGET_TYPE_XSK,
            .Target = Socket,
        },
    };

    if (auto Result =
            XdpApi->XdpCreateProgram(IfIndex, &XdpInspectRxL2, 0, XDP_CREATE_PROGRAM_FLAG_NONE, &Rule, 1, &Program);
        FAILED(Result)) {
        LOGERR("XdpCreateProgram failed: %x", Result);
        return EXIT_FAILURE;
    }

    JoinGroupOnAllInterfaces();

    //
    // Continuously scan the RX ring and TX completion ring for new descriptors.
    // For simplicity, this loop performs actions one frame at a time. This can
    // be optimized further by consuming, reserving, and submitting batches of
    // frames across each XskRing* function.
    //
    while (TRUE) {
        if (XskRingConsumerReserve(&RxRing, 1, &RingIndex) == 1) {
            XSK_BUFFER_DESCRIPTOR* RxBuffer;

            //
            // A new RX frame appeared on the RX ring. Forward it to the TX
            // ring.

            RxBuffer = (XSK_BUFFER_DESCRIPTOR*)XskRingGetElement(&RxRing, RingIndex);

            //
            // Swap source and destination fields within the frame payload.
            //
            UCHAR* pFrame = (UCHAR*)Frame;
            TranslateRxToTx(&pFrame[RxBuffer->Address.AddressAndOffset], RxBuffer->Length);

            //
            // Advance the consumer index of the RX ring and the producer index
            // of the TX ring, which allows XDP to write and read the descriptor
            // elements respectively.
            //
            XskRingConsumerRelease(&RxRing, 1);

            //
            // Reserve space in the RX fill ring. Since we're only using one
            // frame in this sample, space is guaranteed to be available.
            //
            XskRingProducerReserve(&RxFillRing, 1, &RingIndex);
            *(UINT32*)XskRingGetElement(&RxFillRing, RingIndex) = 0;
            XskRingProducerSubmit(&RxFillRing, 1);
        }
    }

    //
    // Close the XDP program. Traffic will no longer be intercepted by XDP.
    //
    CloseHandle(Program);

    //
    // Close the AF_XDP socket. All socket resources will be cleaned up by XDP.
    //
    CloseHandle(Socket);

    return EXIT_SUCCESS;
}
