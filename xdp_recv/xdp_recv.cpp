//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

#include <iostream>

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <xdpapi.h>
#include <afxdp_helper.h>

#pragma comment(lib, "xdpapi.lib")

extern void JoinMulticastGroupOnAllInterfaces(const char* group_address = "224.0.0.200");


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
    // LOGERR("htons: %04x -> %04x", value, result);
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
        LOGERR("Length: %u: SrcPort: %04x, DstPort: %04x", Length, htons(srcPort), htons(dstPort));
    }

    memset(Frame, 0, Length);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, UsageText);
        return EXIT_FAILURE;
    }

    UINT32 IfIndex = atoi(argv[1]);

    //
    // Retrieve the XDP API dispatch table.
    //
    const XDP_API_TABLE* XdpApi;
    if (auto Result = XdpOpenApi(XDP_API_VERSION_1, &XdpApi); FAILED(Result)) {
        LOGERR("XdpOpenApi failed: %x", Result);
        return EXIT_FAILURE;
    }

    //
    // Create an AF_XDP socket. The newly created socket is not connected.
    //
    HANDLE Socket;
    if (auto Result = XdpApi->XskCreate(&Socket); FAILED(Result)) {
        LOGERR("XskCreate failed: %x", Result);
        return EXIT_FAILURE;
    }

    //
    // Register our frame buffer(s) with the AF_XDP socket. The registered buffer is
    // available mapped into AF_XDP's address space, and elements of descriptor
    // rings refer to relative offsets from the start of the UMEM.
    //
    DWORD NumChunks = 16;
    DWORD ChunkSize = 16384;
    DWORD TotalSize = NumChunks * ChunkSize;
    LPVOID Frame = VirtualAlloc(NULL, TotalSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (Frame == nullptr) {
        LOGERR("VirtualAlloc failed!");
        return EXIT_FAILURE;
    }

    XSK_UMEM_REG UmemReg {
        .TotalSize = TotalSize,
        .ChunkSize = ChunkSize,
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
    // Request a set of RX and RX fill descriptor rings.
    // XDP will create the rings and map them into the process address space as part
    // of the XskActivate step further below.
    //

    UINT32 RingSize = NumChunks;
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
    // available and RX can occur.
    //
    if (auto Result = XdpApi->XskActivate(Socket, XSK_ACTIVATE_FLAG_NONE); FAILED(Result)) {
        LOGERR("XskActivate failed: %x", Result);
        return EXIT_FAILURE;
    }

    //
    // Retrieve the RX, RX fill, TX, and TX completion ring info from AF_XDP.
    //
    XSK_RING_INFO_SET RingInfo;
    UINT32 OptionLength = sizeof(RingInfo);
    if (auto Result = XdpApi->XskGetSockopt(Socket, XSK_SOCKOPT_RING_INFO, &RingInfo, &OptionLength); FAILED(Result)) {
        LOGERR("XSK_SOCKOPT_RING_INFO failed: %x", Result);
        return EXIT_FAILURE;
    }

    //
    // Initialize the optional AF_XDP helper library with the socket ring info.
    // These helpers simplify manipulation of the shared rings.
    //
    XSK_RING RxRing;
    XSK_RING RxFillRing;
    XskRingInitialize(&RxRing, &RingInfo.Rx);
    XskRingInitialize(&RxFillRing, &RingInfo.Fill);

    //
    // Place an empty frame descriptor into the RX fill ring. When the AF_XDP
    // socket receives a frame from XDP, it will pop the first available
    // frame descriptor from the RX fill ring and copy the frame payload into
    // that descriptor's buffer.
    //
    UINT32 StartRingIndex;
    if (XskRingProducerReserve(&RxFillRing, NumChunks, &StartRingIndex) != NumChunks) {
        LOGERR("XskRingProducerReserve failed to get all descriptors");
        return EXIT_FAILURE;
    }

    for (DWORD i = 0; i < NumChunks; i++) {
        //
        // The value of each RX fill and TX completion ring element is an offset
        // from the start of the UMEM to the start of the frame. Since this sample
        // is using a single buffer, the offset is always zero.
        //
        *(UINT32*)XskRingGetElement(&RxFillRing, StartRingIndex + i) = i * ChunkSize;
        std::cout << "RingIndex + " << i << ": " << i * ChunkSize << std::endl;
    }

    XskRingProducerSubmit(&RxFillRing, NumChunks);

    //
    // Create an XDP program using the parsed rule at the L2 inspect hook point.
    // The rule intercepts all UDP frames destined to local port Pattern.Port and
    // redirects them to the AF_XDP socket.
    //
    XDP_RULE Rule[] {
        {
            .Match = XDP_MATCH_UDP, // XDP_MATCH_UDP_DST
            .Pattern =
                {
                    .Port = htons(0x4321),
                },
            .Action = XDP_PROGRAM_ACTION_REDIRECT,
            .Redirect =
                {
                    .TargetType = XDP_REDIRECT_TARGET_TYPE_XSK,
                    .Target = Socket,
                },
        },
    };

    HANDLE Program;
    if (auto Result = XdpApi->XdpCreateProgram(
            IfIndex, &XdpInspectRxL2, 0, XDP_CREATE_PROGRAM_FLAG_ALL_QUEUES, &Rule[0], (DWORD)std::size(Rule), &Program);
        FAILED(Result)) {
        LOGERR("XdpCreateProgram failed: %x", Result);
        return EXIT_FAILURE;
    }

    JoinMulticastGroupOnAllInterfaces();

    //
    // Continuously scan the RX ring and TX completion ring for new descriptors.
    // For simplicity, this loop performs actions one frame at a time. This can
    // be optimized further by consuming, reserving, and submitting batches of
    // frames across each XskRing* function.
    //
    while (TRUE) {
        if (XskRingConsumerReserve(&RxRing, 1, &StartRingIndex) == 1) {
            XSK_BUFFER_DESCRIPTOR* RxBuffer;

            //
            // A new RX frame appeared on the RX ring. Forward it to the TX
            // ring.

            RxBuffer = (XSK_BUFFER_DESCRIPTOR*)XskRingGetElement(&RxRing, StartRingIndex);

            //
            // Swap source and destination fields within the frame payload.
            //
            UCHAR* pFrame = (UCHAR*)Frame;
            std::cout << "AddressAndOffset: " << *(UINT32*)(RxBuffer) << std::endl;
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
            XskRingProducerReserve(&RxFillRing, 1, &StartRingIndex);
            *(UINT32*)XskRingGetElement(&RxFillRing, StartRingIndex) = *(UINT32*)RxBuffer;
            XskRingProducerSubmit(&RxFillRing, 1);

            static DWORD counter = 0;
            if (counter++ > NumChunks)
                break;
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
