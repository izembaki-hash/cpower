/**
 * @file verify_headers.c
 * @brief Pure C11 strict compilation and struct layout verification for all cpower headers.
 */

#include "cpower.h"
#include "common.h"
#include "perception.h"
#include "input.h"
#include "ipc.h"
#include "security.h"
#include <stdio.h>

/* Verify cache-line isolation of ShmRingBufferHeader */
_Static_assert(sizeof(ShmRingBufferHeader) == 192, "ShmRingBufferHeader must equal exactly 3 cache lines (192 bytes)");
_Static_assert(offsetof(ShmRingBufferHeader, writeIdx) == 64, "writeIdx must start on 2nd 64-byte cache line");
_Static_assert(offsetof(ShmRingBufferHeader, readIdx) == 128, "readIdx must start on 3rd 64-byte cache line");

/* Verify wire packet header sizes */
_Static_assert(sizeof(IPC_FrameHeader) == 8, "IPC_FrameHeader must be 8 bytes");
_Static_assert(sizeof(TreePacketHeader) == 32, "TreePacketHeader must be 32 bytes");

int wmain(void) {
    SemanticNode node;
    SemanticTree tree;
    ImageBuffer img;
    PerceptionConfig pcfg;
    WinEventRecord ev;
    BezierPoint pt;
    TrajectoryConfig tcfg;
    ControlConfig ccfg;
    ShmRingBufferHeader ring;
    CommandPacket cmd;
    SecurityEnvironmentReport sec;

    memset(&node, 0, sizeof(node));
    memset(&tree, 0, sizeof(tree));
    memset(&img, 0, sizeof(img));
    memset(&pcfg, 0, sizeof(pcfg));
    memset(&ev, 0, sizeof(ev));
    memset(&pt, 0, sizeof(pt));
    memset(&tcfg, 0, sizeof(tcfg));
    memset(&ccfg, 0, sizeof(ccfg));
    memset(&ring, 0, sizeof(ring));
    memset(&cmd, 0, sizeof(cmd));
    memset(&sec, 0, sizeof(sec));

    printf("Header Verification Passed!\n");
    printf("  sizeof(SemanticNode)        = %zu\n", sizeof(SemanticNode));
    printf("  sizeof(ShmRingBufferHeader) = %zu\n", sizeof(ShmRingBufferHeader));
    printf("  sizeof(TreePacketHeader)    = %zu\n", sizeof(TreePacketHeader));
    printf("  sizeof(CommandPacket)       = %zu\n", sizeof(CommandPacket));
    printf("  sizeof(SecurityReport)      = %zu\n", sizeof(SecurityEnvironmentReport));
    return 0;
}
