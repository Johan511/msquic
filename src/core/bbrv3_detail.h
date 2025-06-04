#pragma once

#include "precomp.h"

typedef struct BBRV3_RATE_SAMPLE {

    Bytes NewlyAcked; // Newly acknowledged bytes in this ACK event

    // Bytes delivered over the connection prior to this packet being sent
    Bytes PriorDelivered;

    BOOLEAN IsAppLimited;

    // Bytes in flight at the time of the transmission of the packet 
    Bytes TxInFlight;

    TimeDiff Rtt; // Round trip time sample in microseconds

    // Number of bytes delivered between packet being sent and now
    Bytes Delivered;

    // Bandwidth sample in bytes per us based on the rate sample event
    Bandwidth DeliveryRate;

} BBRV3_RATE_SAMPLE;

void BbrV3HandleRestartFromIdle(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
);

void BbrV3UpdateModelAndState(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const BBRV3_RATE_SAMPLE* RateSample
);

void BbrV3UpdateControlParameters(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const BBRV3_RATE_SAMPLE* RateSample
);

void BbrV3ResetCongestionSignals(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
);

void BbrV3ResetLowerBounds(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
);

void BbrV3InitRoundCounting(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
);

void BbrV3ResetFullBW(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
);

void BbrV3InitPacingRate(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
);

void BbrV3EnterStartup(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3
);

BOOLEAN BbrV3GenerateRateSampleFromAck(
    _In_ QUIC_CONGESTION_CONTROL_BBRV3* BbrV3,
    _In_ const QUIC_ACK_EVENT* AckEvent,
    _Out_ BBRV3_RATE_SAMPLE* RateSample
);
