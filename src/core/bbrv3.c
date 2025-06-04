#include "bbrv3_detail.h"
#include "bbrv3.h"

const TimeDiff SRTT = 333; // Initial smoothed RTT in microseconds (333ms)

void BbrV3CongestionControlOnDataSent(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ uint32_t NumRetransmittableBytes
)
{
    BbrV3HandleRestartFromIdle(&Cc->BbrV3);
    Cc->BbrV3.Pipe += NumRetransmittableBytes;
}

BOOLEAN BbrV3CongestionControlOnDataAcknowledged(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ const QUIC_ACK_EVENT* AckEvent
)
{
    QUIC_CONGESTION_CONTROL_BBRV3* BbrV3 = &Cc->BbrV3;
    BBRV3_RATE_SAMPLE RateSample;

    BbrV3GenerateRateSampleFromAck(BbrV3, AckEvent, &RateSample);
    
    BbrV3->Pipe -= AckEvent->NumRetransmittableBytes;
    BbrV3->Delivered += AckEvent->NumRetransmittableBytes;

    BbrV3UpdateModelAndState(BbrV3, &RateSample);
    BbrV3UpdateControlParameters(BbrV3, &RateSample);

    return TRUE;
}

static void BbrV3NoteLoss(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    if(!BbrV3->LossInRound)
        // Note that this is the first loss in the round trip
        BbrV3->LossRoundDelivered = BbrV3->Delivered /* C.delivered */;
    BbrV3->LossInRound = TRUE;
}

void BbrV3CongestionControlOnDataLost(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ const QUIC_LOSS_EVENT* LossEvent
)
{
    UNREFERENCED_PARAMETER(LossEvent);
    // TODO: We arent really handling loss right now
    QUIC_CONGESTION_CONTROL_BBRV3* BbrV3 = &Cc->BbrV3;
    // BBRV3_RATE_SAMPLE RateSample;

    // Should we update Pipe and delivered here?

    BbrV3NoteLoss(BbrV3);
    if(!BbrV3->BandwidthProbeSamples)
        return; // If we are not probing bandwidth, we do not update model

    /*
    RateSample.TxInFlight = 0; // InFlight when Loss packet transmission occurred
    RateSample.Lost = 0; // Data lost since transmit
    RateSample.IsAppLimited = FALSE; // Packet.IsAppLimited, was it app limited when it was sent
    if(BbrV3InFlightTooHigh(BbrV3)) {
        RateSample.TxInFlight = BbrV3InFlightLongtermFromLostPacket(BbrV3, RateSample, Packet);
        BbrV3HandleInflightTooHigh(RateSample);
    }
    */
}

BOOLEAN BbrV3CongestionControlCanSend(
    _In_ QUIC_CONGESTION_CONTROL* Cc
)
{
    QUIC_CONGESTION_CONTROL_BBRV3* BbrV3 = &Cc->BbrV3;

    return BbrV3->CongestionWindow > BbrV3->Pipe;
}

void BbrV3CongestionControlSetExemption(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ uint8_t NumPackets
)
{
    // Increase Congestion window to atleast NumPackets * SMSS?
    UNREFERENCED_PARAMETER(Cc);
    UNREFERENCED_PARAMETER(NumPackets);
}

void BbrV3CongestionControlReset(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ BOOLEAN FullReset
)
{
    UNREFERENCED_PARAMETER(FullReset);
    BbrV3CongestionControlInitialize(Cc, NULL);
}

uint32_t BbrV3CongestionControlGetCongestionWindow(
    _In_ const QUIC_CONGESTION_CONTROL* Cc
)
{
    return (uint32_t)Cc->BbrV3.CongestionWindow;
}

BOOLEAN BbrV3CongestionControlOnDataInvalidated(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ uint32_t NumRetransmittableBytes
)
{
    // This function is not used in the current implementation
    UNREFERENCED_PARAMETER(Cc);
    UNREFERENCED_PARAMETER(NumRetransmittableBytes);
    return 0;
}

BOOLEAN BbrV3CongestionControlOnSpuriousCongestionEvent(
    _In_ QUIC_CONGESTION_CONTROL* Cc
)
{
    // This function is not used in the current implementation
    UNREFERENCED_PARAMETER(Cc);
    return FALSE;
}

uint32_t BbrV3CongestionControlGetSendAllowance(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_ uint64_t TimeSinceLastSend,
    _In_ BOOLEAN TimeSinceLastSendValid
)
{
    UNREFERENCED_PARAMETER(TimeSinceLastSend);
    UNREFERENCED_PARAMETER(TimeSinceLastSendValid);

    QUIC_CONGESTION_CONTROL_BBRV3* BbrV3 = &Cc->BbrV3;

    if(BbrV3->Pipe >= BbrV3->CongestionWindow) {
        // If pipe is already full, we cannot send anything
        return 0;
    }

    uint64_t SendAllowance = (uint64_t)(BbrV3->CongestionWindow - BbrV3->Pipe);
    // TODO: Use send quantum, pacing
    
    return (uint32_t)SendAllowance;
}

uint32_t BbrV3CongestionControlGetBytesInFlightMax(
    _In_ const QUIC_CONGESTION_CONTROL* Cc
)
{
    return (uint32_t)Cc->BbrV3.MaxInFlight;
}

uint8_t BbrV3CongestionControlGetExemptions(
    _In_ const QUIC_CONGESTION_CONTROL* Cc
)
{
    // This function is not used in the current implementation
    UNREFERENCED_PARAMETER(Cc);
    return 0;
}

BOOLEAN BbrV3CongestionControlIsAppLimited(
    _In_ const QUIC_CONGESTION_CONTROL* Cc
)
{
    return Cc->BbrV3.IsAppLimited;
}

void BbrV3CongestionControlSetAppLimited(
    _In_ QUIC_CONGESTION_CONTROL* Cc
)
{
    Cc->BbrV3.IsAppLimited = TRUE;
}

void BbrV3CongestionControlLogOutFlowStatus(
    _In_ const QUIC_CONGESTION_CONTROL* Cc
)
{
    UNREFERENCED_PARAMETER(Cc);
}

static const QUIC_CONGESTION_CONTROL QuicCongestionControlBbrV3 = {
    .Name = "BBRV3",
    .QuicCongestionControlCanSend = BbrV3CongestionControlCanSend,
    .QuicCongestionControlSetExemption = BbrV3CongestionControlSetExemption,
    .QuicCongestionControlReset = BbrV3CongestionControlReset,
    .QuicCongestionControlGetSendAllowance = BbrV3CongestionControlGetSendAllowance,
    .QuicCongestionControlGetCongestionWindow = BbrV3CongestionControlGetCongestionWindow,
    .QuicCongestionControlOnDataSent = BbrV3CongestionControlOnDataSent,
    .QuicCongestionControlOnDataInvalidated = BbrV3CongestionControlOnDataInvalidated,
    .QuicCongestionControlOnDataAcknowledged = BbrV3CongestionControlOnDataAcknowledged,
    .QuicCongestionControlOnDataLost = BbrV3CongestionControlOnDataLost,
    .QuicCongestionControlOnEcn = NULL,
    .QuicCongestionControlOnSpuriousCongestionEvent = BbrV3CongestionControlOnSpuriousCongestionEvent,
    .QuicCongestionControlLogOutFlowStatus = BbrV3CongestionControlLogOutFlowStatus,
    .QuicCongestionControlGetExemptions = BbrV3CongestionControlGetExemptions,
    .QuicCongestionControlGetBytesInFlightMax = BbrV3CongestionControlGetBytesInFlightMax,
    .QuicCongestionControlIsAppLimited = BbrV3CongestionControlIsAppLimited,
    .QuicCongestionControlSetAppLimited = BbrV3CongestionControlSetAppLimited,
};

void BbrV3CongestionControlInitializeImpl(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_opt_ const QUIC_SETTINGS_INTERNAL* Settings,
    _In_ TimePoint (*now)(void)
)
{
    UNREFERENCED_PARAMETER(Settings);
    *Cc = QuicCongestionControlBbrV3;
    Cc->BbrV3.now = now;

    QUIC_CONGESTION_CONTROL_BBRV3* BbrV3 = &Cc->BbrV3;
    BbrV3->MaxBandwidthFilter[0] = 0;
    BbrV3->MaxBandwidthFilter[1] = 0;
    BbrV3->CycleCount = 0;

    BbrV3->MinRtt = SRTT;
    BbrV3->MinRttTimeStamp = BbrV3->now();

    BbrV3->ProbeRttDoneStamp = 0;
    BbrV3->ProbeRttRoundDone = FALSE;

    BbrV3->PriorCongestionWindow = 0;
    BbrV3->IdleRestart = FALSE;

    // TODO: Extra Acked Interval

    BbrV3->FullBandwidthReached = FALSE;

    BbrV3ResetCongestionSignals(BbrV3);
    BbrV3ResetLowerBounds(BbrV3);
    BbrV3InitRoundCounting(BbrV3);
    BbrV3ResetFullBW(BbrV3);
    BbrV3InitPacingRate(BbrV3);
    BbrV3EnterStartup(BbrV3);
}

void BbrV3CongestionControlInitialize(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_opt_ const QUIC_SETTINGS_INTERNAL* Settings
)
{
    BbrV3CongestionControlInitializeImpl(Cc, Settings, &BbrV3NowUs);
}
