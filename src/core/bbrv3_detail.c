#include "bbrv3_detail.h"
#include "bbrv3.h"

// To be decided constants
const Bytes InitialCongestionWindow = 10 * 1024; // Initial congestion window in bytes
const Bytes BbrV3MinPipeCongestionWindow = 2 * 10 * 1024 /* InitialCongestionWindow */; // Minimum pipe congestion window in bytes
const Bytes SMSS = 1460; // Standard Maximum Segment Size in bytes
const double BBRHeadroom = 0.15;
const TimeDiff MinRTTFilterLen = 10; // Minimum RTT filter length in milliseconds
const TimeDiff ProbeRTTInterval = 100; // Probe RTT interval in milliseconds
const double BBRProbeRTTCwndGain = 0.5;
const TimeDiff ProbeRTTDuration = 200; // Probe RTT duration in milliseconds
const double BBRBeta = 0.7;
#define INFINITY ((Bytes) -1) // Define infinity for Bytes type

TimePoint BbrV3NowUs(void)
{
    return CxPlatTimeUs64();
};

BOOLEAN IsProbeBw(
    _In_ BBRV3_STATE State
)
{
    // TODO: Risky?
    return (State >= PROBE_BW_DOWN) & (State <= PROBE_BW_UP);
}

BOOLEAN IsProbeRtt(
    _In_ BBRV3_STATE State
)
{
    return (State == PROBE_RTT);
}

BOOLEAN BbrV3HasElapsedInPhase(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ TimeDiff PhaseDurationUs
)
{
    return (BbrV3->now() - BbrV3->CycleStamp) >= (PhaseDurationUs);
}

void BbrV3StartRound(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    BbrV3->NextRoundDelivered = BbrV3->Delivered /* C.delivered */;
}

void Bbrv3UpdateRound(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const BBRV3_RATE_SAMPLE* RateSample
) 
{
    if(RateSample->Delivered /* packet.delivered */ >= BbrV3->NextRoundDelivered) {
        BbrV3StartRound(BbrV3);
        BbrV3->RoundCount++;
        BbrV3->RoundsSinceBandwidthProbe++;
        BbrV3->RoundStart = TRUE;
    }
    else {
        BbrV3->RoundStart = FALSE;
    }
}

void BbrV3ResetCongestionSignals(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    BbrV3->LossInRound = FALSE;
    BbrV3->BandwidthLatest = 0;
    BbrV3->InFlightLatest = 0;
}

void BbrV3ResetLowerBounds(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    BbrV3->BandwidthShortTerm = INFINITY;
    BbrV3->InFlightShortTerm = INFINITY;
}

void UpdateMaxBandwidthWindow(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ Bandwidth NewMaxBandwidth
)
{
    BbrV3->MaxBandwidthFilter[BbrV3->CycleCount] = NewMaxBandwidth;
}

Bandwidth GetMaxBandwidth(
    _In_ const QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    return CXPLAT_MAX(
        BbrV3->MaxBandwidthFilter[0],
        BbrV3->MaxBandwidthFilter[1]); // only two slots in the window
}

void BbrV3UpdateMaxBandwidth(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const BBRV3_RATE_SAMPLE* RateSample
)
{
    Bbrv3UpdateRound(BbrV3, RateSample);
    if(RateSample->DeliveryRate >= GetMaxBandwidth(BbrV3) || !RateSample->IsAppLimited)
        UpdateMaxBandwidthWindow(BbrV3, RateSample->DeliveryRate);
}


void BbrV3AdvanceMaxBandwidthFilter(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    // We need to increment cycle count modulo size of bandwidth filter
    // Bandwidth filter is a circular buffer of size 2
    // so we can use a boolean to toggle between the two slots
    BbrV3->CycleCount = !BbrV3->CycleCount;
}

void BbrV3RaiseInFlightLongTermSlope(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    uint64_t GrowthThisRound =  ((uint64_t)SMSS) << BbrV3->BandwidthProbeUpRounds;

    // Function is only called once per round
    // so we increment the number of rounds over here
    BbrV3->BandwidthProbeUpRounds = CXPLAT_MIN(BbrV3->BandwidthProbeUpRounds + 1, 30);
    BbrV3->ProbeUpCount = (uint64_t)CXPLAT_MAX(BbrV3->CongestionWindow / GrowthThisRound, 1.);
}

void BbrV3ResetFullBandwidth(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    BbrV3->FullBandwidth = 0;
    BbrV3->FullBandwidthCount = 0;
    BbrV3->FullBandwidthNow = FALSE;
}

void BbrV3StartProbeBW_DOWN(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    // Any congestion signals we have maintained are discarded
    BbrV3ResetCongestionSignals(BbrV3);

    // prevents growth of InFlightLongTerm
    BbrV3->ProbeUpCount = (uint64_t)(-1);

    /* Pick ProbeBwWait randomly based on OR of 2 conditions */
    // Pick number of rounds to wait
    BbrV3->RoundsSinceBandwidthProbe = rand() & 1; // Randomly pick between 0 and 1
    // Pick time to wait, picks a random value between 2 and 3 seconds
    BbrV3->BandwidthProbeWait = (time_t)(2 + ((double)rand() / RAND_MAX)) * 1000; 
    
    // Time stamp to identify when to start probing bandwidth again
    // we probe after CycleStamp + BandwidthProbeWait
    BbrV3->CycleStamp = BbrV3->now();
    BbrV3->AckPhase = ACKS_PROBE_STOPPING;
    BbrV3StartRound(BbrV3);
    BbrV3->State = PROBE_BW_DOWN;
}

void BbrV3StartProbeBW_CRUISE(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    BbrV3->State = PROBE_BW_CRUISE;
}

void BbrV3StartProbeBW_REFILL(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    BbrV3ResetLowerBounds(BbrV3);
    BbrV3->BandwidthProbeUpRounds = 0;
    BbrV3->BandwidthProbeUpAcks = 0;
    BbrV3->AckPhase = ACKS_REFILLING;
    BbrV3StartRound(BbrV3);
    BbrV3->State = PROBE_BW_REFILL;
}

void BbrV3StartProbeBW_UP(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const BBRV3_RATE_SAMPLE* RateSample
)
{
    BbrV3->AckPhase = ACKS_PROBE_STARTING;
    BbrV3StartRound(BbrV3);
    BbrV3ResetFullBandwidth(BbrV3);
    BbrV3->FullBandwidth = RateSample->DeliveryRate;
    BbrV3->State = PROBE_BW_UP;
    BbrV3RaiseInFlightLongTermSlope(BbrV3);
}

void BbrV3EnterStartup(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    BbrV3->State = STARTUP;
    BbrV3->PacingGain = BBRStartupPacingGain;
    BbrV3->CongestionWindowGain = BBRDefaultCwndGain;
}

void BbrV3EnterDrain(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    BbrV3->State = DRAIN;
    BbrV3->PacingGain = BBRDrainPacingGain;
    BbrV3->CongestionWindowGain = BBRDefaultCwndGain;
}


void BbrV3EnterProbeBandwidth(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    BbrV3->CongestionWindowGain = BBRDefaultCwndGain;
    BbrV3StartProbeBW_DOWN(BbrV3);
}

void BbrV3EnterProbeRtt(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    BbrV3->State = PROBE_RTT;
    BbrV3->PacingGain = 1.0;
    BbrV3->CongestionWindowGain = 0.5;
}

// Saving our precious congestion window estimate before messing with it
void BbrV3SaveCwnd(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    if(/* !BbrV3InLossRecovery(BbrV3) && */ !IsProbeRtt(BbrV3->State)) 
        BbrV3->PriorCongestionWindow = BbrV3->CongestionWindow;
    else
        BbrV3->PriorCongestionWindow = CXPLAT_MAX(BbrV3->PriorCongestionWindow, BbrV3->CongestionWindow);
}

// Restoring our precious congestion window estimate after we are done messing with it
void BbrV3RestoreCwnd(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    BbrV3->CongestionWindow = CXPLAT_MAX(BbrV3->CongestionWindow, BbrV3->PriorCongestionWindow);
}

Bytes BbrV3BdpMultiple(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ double Gain
)
{
    if(BbrV3->MinRtt == INFINITY)
        // If we don't have a valid min RTT, use the initial congestion window
        return InitialCongestionWindow;

    BbrV3->BandwidthDelayProduct = BbrV3->Bandwidth * BbrV3->MinRtt;
    return (BbrV3->BandwidthDelayProduct * Gain);
}

Bytes BbrV3QuantizationBudget(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ Bytes InFlight
)
{
    // BbrV3UpdateOffloadBudget(BbrV3);
    // InFlight = CXPLAT_MAX(InFlight, BbrV3->OffloadBudget);
    InFlight = CXPLAT_MAX(InFlight, BbrV3MinPipeCongestionWindow);
    if(BbrV3->State == PROBE_BW_UP)
        InFlight += 2 * SMSS;
    return InFlight;
}

Bytes BbrV3InFlight(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ double Gain
)
{
    Bytes InFlight = BbrV3BdpMultiple(BbrV3, Gain);
    return BbrV3QuantizationBudget(BbrV3, InFlight);
}

void BbrV3UpdateLatestDeliverySignals(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const BBRV3_RATE_SAMPLE* RateSample
)
{
   BbrV3->LossRoundStart = FALSE;

   // Updating latest delivery signals based on the rate sample
   BbrV3->BandwidthLatest = CXPLAT_MAX(BbrV3->BandwidthLatest, RateSample->DeliveryRate);
   BbrV3->InFlightLatest = CXPLAT_MAX(BbrV3->InFlightLatest, RateSample->Delivered);

   if(RateSample->PriorDelivered >= BbrV3->LossRoundDelivered) {
        BbrV3->LossRoundDelivered = BbrV3->Delivered /* C.delivered */;
        BbrV3->LossRoundStart = TRUE;
   }
}

void BbrV3InitLowerBounds(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    if(BbrV3->BandwidthShortTerm == INFINITY)
        BbrV3->BandwidthShortTerm = GetMaxBandwidth(BbrV3);

    if(BbrV3->InFlightShortTerm == INFINITY)
        BbrV3->InFlightShortTerm = BbrV3->CongestionWindow;
}

void BbrV3LossLowerBounds(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    BbrV3->BandwidthShortTerm = CXPLAT_MAX(BbrV3->BandwidthLatest, BBRBeta * BbrV3->BandwidthShortTerm);
    BbrV3->InFlightShortTerm = CXPLAT_MAX(BbrV3->InFlightLatest, BBRBeta * BbrV3->InFlightShortTerm);
}

void BbrV3AdaptLowerBoundsFromCongestion(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    if(IsProbeBw(BbrV3->State))
        return;

    // If we are in loss, we adapt lower bounds
    if(BbrV3->LossInRound) {
        BbrV3InitLowerBounds(BbrV3);
        BbrV3LossLowerBounds(BbrV3);
    }
}

void BbrV3UpdateCongestionSignals(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const BBRV3_RATE_SAMPLE* RateSample
)
{
    BbrV3UpdateMaxBandwidth(BbrV3, RateSample);

    if(!BbrV3->LossRoundStart)
        return;

    // Should be executed only once per round
    // Adapting based on loss in previous round
    BbrV3AdaptLowerBoundsFromCongestion(BbrV3);

    // Reset loss in round
    BbrV3->LossInRound = 0;
}

void BbrV3CheckFullBWReached(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const BBRV3_RATE_SAMPLE* RateSample
)
{
    if(BbrV3->FullBandwidthNow /* Already have asserted we are in FullBw */

        || !BbrV3->RoundStart /* We have already checked using a newer sample, 
            RoundStart is set in UpdateMaxBandwidth if this is a newer rate sample  */

        || RateSample->IsAppLimited /* If RateSample was obtained when App is rate limited, 
            we can not expect at 1.25x growth */)
        return;

    // If bandwidth has grown significantly (atleast 1.25x), we know FullBandwidth is not reached
    if(RateSample->DeliveryRate >= BbrV3->FullBandwidth * 1.25) {
        BbrV3ResetFullBandwidth(BbrV3);
        // record new delivery rate as baseline for full bandwidth
        BbrV3->FullBandwidth = RateSample->DeliveryRate;
        return;
    }

    BbrV3->FullBandwidthCount++; // Another round without much growth
    // if we hit >= threshold, we consider it full bandwidth
    BbrV3->FullBandwidthNow = (BbrV3->FullBandwidthCount >= BBRFullBandwidthCountThreshold);

    // Set full bandwidth reached flag to indicate we hit FullBw atleast once in the connection lifetime
    BbrV3->FullBandwidthReached |= BbrV3->FullBandwidthNow;
}

void BbrV3CheckStartupDone(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const BBRV3_RATE_SAMPLE* RateSample
)
{
    UNREFERENCED_PARAMETER(RateSample);
    // BbrV3CheckStartupHighLoss(BbrV3, RateSample);
    if(BbrV3->State == STARTUP && BbrV3->FullBandwidthReached)
        BbrV3EnterDrain(BbrV3);
}

void BbrV3CheckDrainDone(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    if(BbrV3->State == DRAIN && BbrV3->Pipe < BbrV3InFlight(BbrV3, 1.0))
        BbrV3EnterProbeBandwidth(BbrV3);
}

BOOLEAN BbrV3IsTimeToProbeBandwidth(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    if(BbrV3HasElapsedInPhase(BbrV3, BbrV3->BandwidthProbeWait) 
        || FALSE /* BbrV3IsRenoCoexistenceProbeTime(BbrV3) */ )
    {
        BbrV3StartProbeBW_REFILL(BbrV3);
        return TRUE;
    }
    return FALSE;
}

void BbrV3ProbeInflightLongtermUpward(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    // We want to increase InFlightLongTerm if we feel we can get away with sending more data
    // But we are currently not sending more data, so no valid signal to increase it
    if(BbrV3->CongestionWindow < BbrV3->InFlightLongTerm 
        || FALSE /* !BbrV3->IsCongestionWindowLimited, never written to */) {
        return;
    }

    BbrV3->BandwidthProbeUpAcks += 1; // Increment the number of acks in the current probe up

    if(BbrV3->BandwidthProbeUpAcks >= BbrV3->ProbeUpCount) {
        uint64_t Delta = (BbrV3->BandwidthProbeUpAcks / BbrV3->ProbeUpCount);
        BbrV3->BandwidthProbeUpAcks -= Delta * BbrV3->ProbeUpCount;
        // Each ACK leads to a growth of approximately Growth / CongestionWindow
        // When we send CongestionWindow bytes, we expect to growth of GrowthThisRound bytes
        BbrV3->InFlightLongTerm += Delta;
    }

    // Once per round we increase the slope of InFlightLongTerm
    // TODO: shouldn't this be before the previous if?
    if(BbrV3->RoundStart) {
        BbrV3RaiseInFlightLongTermSlope(BbrV3);
    }
}

void BbrV3AdaptUpperBounds(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const BBRV3_RATE_SAMPLE* RateSample
)
{
    if(BbrV3->AckPhase == ACKS_PROBE_STARTING && BbrV3->RoundStart) {
        // Starting to get Bandwidth Probe samples
        // BbrV3->AckPhase is set to ACKS_PROBE_STARTING in BbrV3StartProbeBW_UP, 
        // which is called after this function
        BbrV3->AckPhase = ACKS_PROBE_FEEDBACK;
    }

    if(BbrV3->AckPhase == ACKS_PROBE_STOPPING && BbrV3->RoundStart) {
        // BbrV3->AckPhase is set to `ACKS_PROBE_STOPPING` in BbrV3CheckProbeRTT,
        // which is called before this function
        if(IsProbeBw(BbrV3->State) && !RateSample->IsAppLimited)
            BbrV3AdvanceMaxBandwidthFilter(BbrV3);
    }

    if(/* !BbrV3InflightTooHigh(BbrV3) */ TRUE) {
        if(BbrV3->InFlightLongTerm == INFINITY)
            // can not raise it further
            return;
        
        // If inflight is not too high, we update the InFlightLongTerm to latest TxInFlight
        if(RateSample->TxInFlight > BbrV3->InFlightLongTerm)
            BbrV3->InFlightLongTerm = RateSample->TxInFlight;

        // In flight is not too high and we are in PROBE_BW_UP state
        // helps simulate the exponential growth of congestion window
        if(BbrV3->State == PROBE_BW_UP) {
            BbrV3ProbeInflightLongtermUpward(BbrV3);
        }
    }
}

Bytes BbrV3InFlightWithHeadroom(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    if(BbrV3->InFlightLongTerm == INFINITY) {
        return INFINITY;
    }

    Bytes Headroom = CXPLAT_MAX(SMSS, BBRHeadroom * BbrV3->InFlightLongTerm);
    return CXPLAT_MAX(BbrV3->InFlightLongTerm - Headroom, BbrV3MinPipeCongestionWindow);
}


BOOLEAN BbrV3IsTimeToCruise(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    if(BbrV3->Pipe > BbrV3InFlightWithHeadroom(BbrV3))
        return FALSE; // not enough headroom, not time to cruise
    if(BbrV3->Pipe <= BbrV3InFlight(BbrV3, 1.0))
        return TRUE;  // inflight <= estimated BDP, time to cruise
    // if we are here, it means that we have enough headroom
    // But more than the estimated BDP, let us let it cruise
    return TRUE;
}

BOOLEAN BbrV3IsTimeToGoDown(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const BBRV3_RATE_SAMPLE* RateSample
)
{
    if(/* BbrV3->IsCongestionWindowLimited */ TRUE && BbrV3->CongestionWindow >= BbrV3->InFlightLongTerm) {
        // We have not yet reached FullBandwidth, so we can not go down
        // We use latest rate sample as full bandwidth
        BbrV3ResetFullBandwidth(BbrV3);
        BbrV3->FullBandwidth = RateSample->DeliveryRate;
    } else if (BbrV3->FullBandwidthNow) {
        return TRUE;
    }
    return FALSE;
}


void BbrV3UpdateProbeBWCyclePhase(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const BBRV3_RATE_SAMPLE* RateSample
)
{
    if(!BbrV3->FullBandwidthReached)
        // we only handle steady state behavior
        return;

    // Updates max bandwidth filter and inflight long term,
    // both of which are used as upper bounds for the model
    BbrV3AdaptUpperBounds(BbrV3, RateSample);

    switch(BbrV3->State) {

        case PROBE_BW_DOWN:
            if(BbrV3IsTimeToProbeBandwidth(BbrV3))
                return;
            if(BbrV3IsTimeToCruise(BbrV3))
                BbrV3StartProbeBW_CRUISE(BbrV3);
            break;

        case PROBE_BW_CRUISE:
            if(BbrV3IsTimeToProbeBandwidth(BbrV3))
                return;
            break;

        case PROBE_BW_REFILL:
            if(BbrV3->RoundStart) {
                BbrV3->BandwidthProbeSamples = 1;
                BbrV3StartProbeBW_UP(BbrV3, RateSample);
            }
            break;

        case PROBE_BW_UP:
            if(BbrV3IsTimeToGoDown(BbrV3, RateSample))
                BbrV3StartProbeBW_DOWN(BbrV3);
            break;

        default:
            // handling only probe bandwidth state
            // No other states are handled
            break;
    }
}

void BbrV3UpdateMinRTT(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const BBRV3_RATE_SAMPLE* RateSample
)
{
    BbrV3->ProbeRttMinExpired = BbrV3->now() > (BbrV3->ProbeRttMinTimeStamp + ProbeRTTInterval);

    if(BbrV3->ProbeRttMinExpired || RateSample->Rtt < BbrV3->ProbeRttMin) {
        // If our ProbeRttMin sample expired or we got a lower RTT sample, update RttMin
        BbrV3->ProbeRttMin = RateSample->Rtt;
        BbrV3->ProbeRttMinTimeStamp = BbrV3->now();
    }

    BOOLEAN MinRttExpired = BbrV3->now() > (BbrV3->MinRttTimeStamp + MinRTTFilterLen);
    if(MinRttExpired || BbrV3->ProbeRttMin < BbrV3->MinRtt) {
        // If out MinRtt sample expired or we got a lower ProbeRttMin sample, update MinRtt
        BbrV3->MinRtt = BbrV3->ProbeRttMin;
        BbrV3->MinRttTimeStamp = BbrV3->now();
    }
}

Bytes BbrV3ProbeRttCwnd(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    Bytes ProbeRttCwnd = BbrV3BdpMultiple(BbrV3, BBRProbeRTTCwndGain);
    ProbeRttCwnd = CXPLAT_MAX(ProbeRttCwnd, BbrV3MinPipeCongestionWindow);
    return ProbeRttCwnd;
}

void BbrV3ExitProbeRtt(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    BbrV3ResetLowerBounds(BbrV3);
    if(BbrV3->FullBandwidthReached) {
        BbrV3StartProbeBW_DOWN(BbrV3);
        // since the connection is exiting ProbeRTT
        // we know that inflight is already below the estimated BDP
        // so the connection can proceed immediately to ProbeBW_CRUISE.
        BbrV3StartProbeBW_CRUISE(BbrV3);
    } else {
        BbrV3EnterStartup(BbrV3);
    }
}

void BbrV3CheckProbeRTTDone(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    if(BbrV3->ProbeRttDoneStamp != 0 && BbrV3->now() > BbrV3->ProbeRttDoneStamp) {
        BbrV3->ProbeRttDoneStamp = BbrV3->now();
        BbrV3RestoreCwnd(BbrV3);
        BbrV3ExitProbeRtt(BbrV3);
    }
}

void BbrV3HandleProbeRtt(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    // We want to ignore rate samples in ProbeRtt, because we are reducing rate on purpose
    // We achieve this by marking the connection as AppLimited
    BbrV3->IsAppLimited = TRUE;

    if(BbrV3->ProbeRttDoneStamp == 0 && BbrV3->Pipe <= BbrV3ProbeRttCwnd(BbrV3)) {

        BbrV3->ProbeRttDoneStamp = BbrV3->now() + ProbeRTTDuration;
        // We want to wait until start of next round before we end ProbeRtt
        BbrV3->ProbeRttRoundDone = FALSE;

    } else if (BbrV3->ProbeRttDoneStamp != 0) {

        // Waiting for next round to start
        if(BbrV3->RoundStart)
            BbrV3->ProbeRttRoundDone = TRUE;

        if(BbrV3->ProbeRttRoundDone)
            BbrV3CheckProbeRTTDone(BbrV3);

    }
}

void BbrV3CheckProbeRTT(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const BBRV3_RATE_SAMPLE* RateSample
)
{
    if(!IsProbeRtt(BbrV3->State) && BbrV3->ProbeRttMinExpired && !BbrV3->IdleRestart) {
        BbrV3EnterProbeRtt(BbrV3);
        // We need to remember the last good congestion window sample before messing around with it in ProbRtt
        BbrV3SaveCwnd(BbrV3);
        // reset timestamp of when ProbeRtt is done (it has just begun)
        BbrV3->ProbeRttDoneStamp = 0;
        BbrV3->AckPhase = ACKS_PROBE_STARTING;
        BbrV3StartRound(BbrV3);
    }

    if(IsProbeRtt(BbrV3->State))
        BbrV3HandleProbeRtt(BbrV3);

    if(RateSample->Delivered > 0)
        BbrV3->IdleRestart = FALSE;
}

void BbrV3UpdateModelAndState(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const BBRV3_RATE_SAMPLE* RateSample
)
{
    // We track the latest rate sample which will be used in the further calculations
    BbrV3UpdateLatestDeliverySignals(BbrV3, RateSample);

    // Updates max bandwidth filter and adapts lower bound from congestion 
    BbrV3UpdateCongestionSignals(BbrV3, RateSample);

    // TODO: Update ACK aggregation logic
    // BbrV3UpdateACKAggregation(BbrV3);

    // Update state variables associated with checking if we reached full bandwidth
    BbrV3CheckFullBWReached(BbrV3, RateSample);

    // Check if we are in Startup state and enter Drain if we reach full BW (TODO: or observe high loss) 
    BbrV3CheckStartupDone(BbrV3, RateSample);

    // Check if drain is done(and proceed to ProbeBw) by checking if Pipe is below desired InFlight data volume
    BbrV3CheckDrainDone(BbrV3);

    // TODO: describe each ProbeBw state

    // If we are in ProbeBw, do the necessary actions
    BbrV3UpdateProbeBWCyclePhase(BbrV3, RateSample);

    BbrV3UpdateMinRTT(BbrV3, RateSample);

    BbrV3CheckProbeRTT(BbrV3, RateSample);

    // Advancing latest delivery signals
    if(BbrV3->LossRoundStart) {
        BbrV3->BandwidthLatest = RateSample->DeliveryRate;
        BbrV3->InFlightLatest = RateSample->Delivered;
    }

    // Finally compute the bandwidth
    BbrV3->Bandwidth = CXPLAT_MIN(GetMaxBandwidth(BbrV3), BbrV3->BandwidthShortTerm);
}

void BbrV3SetPacingRateWithGain(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ double PacingGain
)
{
    double rate = PacingGain * BbrV3->BandwidthLatest * ((100. - BBRPacingMarginPercent) / 100.);
    if(BbrV3->FullBandwidthReached || rate > BbrV3->PacingRate)
        BbrV3->PacingRate = rate;
    
}

void BbrV3SetSendQuantum(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    BbrV3->SendQuantum = BbrV3->PacingRate * 1000; // 1 second in microseconds
    BbrV3->SendQuantum = CXPLAT_MIN(BbrV3->SendQuantum, 64 * 1024);
    BbrV3->SendQuantum = CXPLAT_MAX(BbrV3->SendQuantum, 2 * SMSS);
}

void BbrV3UpdateMaxInFlight(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    // Without Ack Aggregation, this is same as BbrV3InFlight()

    // BbrV3UpdateAggregationBudget(BbrV3);
    Bytes InFlight = BbrV3BdpMultiple(BbrV3,BbrV3->CongestionWindowGain);
    // InFlight += BbrV3->ExtraAcked;
    BbrV3->MaxInFlight = BbrV3QuantizationBudget(BbrV3, InFlight);
}

void BbrV3BoundCwndForModel(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    Bytes Cap = INFINITY;
    if(IsProbeBw(BbrV3->State) && BbrV3->State != PROBE_BW_CRUISE)
        // In PROBE_BW_DOWN, PROBE_BW_REFILL, PROBE_BW_UP
        Cap = BbrV3->InFlightLongTerm;
    else if(BbrV3->State == PROBE_RTT || BbrV3->State == PROBE_BW_CRUISE)
        // In PROBE_RTT or PROBE_BW_CRUISE
        Cap = BbrV3InFlightWithHeadroom(BbrV3);

    Cap = CXPLAT_MIN(Cap, BbrV3->InFlightShortTerm);
    Cap = CXPLAT_MAX(Cap, BbrV3MinPipeCongestionWindow);
    
    BbrV3->CongestionWindow = CXPLAT_MIN(BbrV3->CongestionWindow, Cap);
}

void BbrV3SetCwnd(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const BBRV3_RATE_SAMPLE* RateSample
)
{
    BbrV3UpdateMaxInFlight(BbrV3);
    if(BbrV3->FullBandwidthReached)
        BbrV3->CongestionWindow = CXPLAT_MIN(BbrV3->CongestionWindow + RateSample->NewlyAcked, BbrV3->MaxInFlight);
    else if (BbrV3->CongestionWindow < BbrV3->MaxInFlight ||
             BbrV3->Delivered < InitialCongestionWindow) {
        BbrV3->CongestionWindow += RateSample->NewlyAcked;
    }

    BbrV3->CongestionWindow =
        CXPLAT_MAX(BbrV3->CongestionWindow, BbrV3MinPipeCongestionWindow);

    if(IsProbeRtt(BbrV3->State)) {
        // If we are in ProbeRTT, we bound the congestion window to the minimum pipe congestion window
        BbrV3->CongestionWindow = CXPLAT_MIN(BbrV3->CongestionWindow, BbrV3ProbeRttCwnd(BbrV3));
    }

    BbrV3BoundCwndForModel(BbrV3);
}

void BbrV3UpdateControlParameters(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const BBRV3_RATE_SAMPLE* RateSample
)
{
    BbrV3SetPacingRateWithGain(BbrV3, BbrV3->PacingGain);
    
    BbrV3SetSendQuantum(BbrV3);

    BbrV3SetCwnd(BbrV3, RateSample);
}

void BbrV3HandleRestartFromIdle(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    // Restart from idle is detected by, was app limited and now pipe is empty
    if(BbrV3->Pipe == 0 && BbrV3->IsAppLimited) {
        BbrV3->IdleRestart = TRUE;
        BBRV3_STATE state = BbrV3->State;
        if(IsProbeBw(state))
            BbrV3SetPacingRateWithGain(BbrV3, 1.0);
        else if(IsProbeRtt(state))
            BbrV3CheckProbeRTTDone(BbrV3);
    }
}

BOOLEAN BbrV3GenerateRateSampleFromAck(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const QUIC_ACK_EVENT* AckEvent,
    _Out_ BBRV3_RATE_SAMPLE* RateSample
)
{
    // Initializing values just in case caller did not initialize them
    RateSample->NewlyAcked = 0;
    RateSample->PriorDelivered = 0;
    
    RateSample->IsAppLimited = AckEvent->IsLargestAckedPacketAppLimited;

    // TODO: very bad estimate of how many bytes in flight when packet is sent, fix it
    RateSample->TxInFlight = (Bytes)(AckEvent->NumRetransmittableBytes - AckEvent->NumTotalAckedRetransmittableBytes);

    if (AckEvent->MinRttValid)
        RateSample->Rtt = AckEvent->MinRtt / 1000;
    else
        RateSample->Rtt = AckEvent->SmoothedRtt / 1000;

    // Update rate sample for each packet acked
    QUIC_SENT_PACKET_METADATA* PacketIter = AckEvent->AckedPackets;
    TimeDiff SendElapsed = 0, AckElapsed = 0;

    while(PacketIter != NULL) {
        // TODO: handle duplicate ACKs
        RateSample->PriorDelivered = CXPLAT_MAX(RateSample->PriorDelivered, PacketIter->TotalBytesSent);
        RateSample->NewlyAcked += PacketIter->PacketLength;

        TimeDiff SendElapsedLocal, AckElapsedLocal = 0;

        CXPLAT_DBG_ASSERT(CxPlatTimeAtOrBefore64(PacketIter->LastAckedPacketInfo.SentTime, PacketIter->SentTime));
        SendElapsedLocal = CxPlatTimeDiff64(PacketIter->LastAckedPacketInfo.SentTime, PacketIter->SentTime);
        
        if (!CxPlatTimeAtOrBefore64(AckEvent->AdjustedAckTime, PacketIter->LastAckedPacketInfo.AdjustedAckTime)) {
            AckElapsed = CxPlatTimeDiff64(PacketIter->LastAckedPacketInfo.AdjustedAckTime, AckEvent->AdjustedAckTime);
        } else {
            AckElapsed = CxPlatTimeDiff64(PacketIter->LastAckedPacketInfo.AckTime, AckEvent->TimeNow);
        }

        // TODO: check this logic
        SendElapsed = CXPLAT_MAX(SendElapsed, SendElapsedLocal);
        AckElapsed = CXPLAT_MAX(AckElapsed, AckElapsedLocal);

        PacketIter = PacketIter->Next;
    }

    RateSample->Delivered = BbrV3->Delivered - RateSample->PriorDelivered;

    TimeDiff Interval = CXPLAT_MAX(SendElapsed, AckElapsed);
    if(Interval == 0)
        return FALSE; // No reliable rate sample

    // Interval was computed in Us, we convert it to ms
    RateSample->DeliveryRate = (double)RateSample->Delivered * 1000 / Interval;

    return TRUE;
}

void BbrV3InitPacingRate(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    Bandwidth NominalBandwidth = InitialCongestionWindow / 1.; // 1ms;
    BbrV3->PacingRate = BBRStartupPacingGain * NominalBandwidth;
}

void BbrV3ResetFullBW(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    BbrV3->FullBandwidth = 0;
    BbrV3->FullBandwidthCount = 0;
    BbrV3->FullBandwidthNow = FALSE;
}

void BbrV3InitRoundCounting(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
)
{
    BbrV3->NextRoundDelivered = 0;
    BbrV3->RoundStart = FALSE;
    BbrV3->RoundCount = 0;
}
