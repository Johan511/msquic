#pragma once

#include <time.h>

typedef double Bytes;
typedef double Bandwidth; // measured in bytes per microsecond
typedef time_t TimePoint;
typedef time_t TimeDiff;

typedef enum BBRV3_STATE {
  STARTUP,
  DRAIN,
  PROBE_BW_DOWN,
  PROBE_BW_CRUISE,
  PROBE_BW_REFILL,
  PROBE_BW_UP,
  PROBE_RTT
} BBRV3_STATE;

typedef enum ACK_PHASE {
  ACKS_PROBE_STARTING,
  ACKS_PROBE_FEEDBACK,
  ACKS_REFILLING,
  ACKS_PROBE_STOPPING,
} ACK_PHASE;

typedef struct QUIC_CONGESTION_CONTROL_BBRV3 {

  BBRV3_STATE State;

  ////////////////////////////////////////////////////////////////////////////////
  // Connection State Variables
  Bytes Pipe; // The amount of data in flight, in bytes.
  Bytes Delivered; // Amount of data delivered over lifetime of the connection in bytes

  BOOLEAN IsAppLimited; // Whether the application is currently limited by the congestion control.

  ////////////////////////////////////////////////////////////////////////////////
  // BBR general algorithm state variables
  BOOLEAN IdleRestart; // Whether the BBR is restarting from an idle state.

  // Startup related parameters
  BOOLEAN FullBandwidthReached; // Whether the full bandwidth has ever been reached over the connection
  BOOLEAN FullBandwidthNow; // Whether the full bandwidth is currently reached.
  Bandwidth FullBandwidth; // The full bandwidth estimate in bytes per microsecond.
  uint64_t FullBandwidthCount; // Number of non-app limited rounds without significant growth in bandwidth. (bounded by BBRFullBandwidthCountThreshold)
  #define BBRFullBandwidthCountThreshold 3

  // Extra Ack related variables
  TimePoint ExtraAckedIntervalStart; // Start time of the extra acked interval.

  // Pacing Related Parameters
  #define BBRPacingMarginPercent 1.0 // static discount factor of 1% used to scale BBR.bw to produce BBR.pacing_rate
  double PacingGain; // The gain factor for pacing rate.
  #define BBRDrainPacingGain 0.35

  // minimum gain value for calculating the pacing rate that will allow the sending rate to double each round (4 * ln(2) ~= 2.77)
  #define BBRStartupPacingGain 2.77 

  // Congestion Window Related Parameters
  #define BBRDefaultCwndGain 2.0 // Default gain for congestion window.
  double CongestionWindowGain; // The gain factor for congestion window.

  // Data rate network path model parameters
  #define MaxBandwidthFilterLength 2
  Bandwidth MaxBandwidthFilter[MaxBandwidthFilterLength];
  BOOLEAN CycleCount; // Which slot of the MaxBandwidthFilter is currently being used (0 or 1).
  Bandwidth BandwidthShortTerm; // Short term bandwidth estimate.
  Bandwidth Bandwidth; // bandwidth estimates for matching the current delivery rate

  // Data Volume network path model parameters
  Bytes InFlightShortTerm; // Long-term maximum volume for acceptable queue pressure
  Bytes InFlightLongTerm; // Long-term maximum volume for acceptable queue pressure
  TimeDiff MinRtt; // Minimum RTT observed in microseconds.
  TimePoint MinRttTimeStamp; // Timestamp when the minimum RTT was observed.
  Bytes BandwidthDelayProduct; // Bandwidth-Delay Product in bytes.
  Bytes MaxInFlight; // Maximum in-flight data to fully utilize the network path
    
  // Congestion response related state
  double BandwidthLatest; // Latest bandwidth estimate.
  Bytes InFlightLatest; // Latest in-flight estimate.

  // Output Control Parameters
  double PacingRate; // The current pacing rate in bytes per microsecond.
  Bytes CongestionWindow; // The current congestion window in bytes.
  Bytes PriorCongestionWindow; // The previous congestion window in bytes.
  Bytes SendQuantum; // Maximum size of data to be scheduled and transmitted together

  // Parameters to schedule Probe RTT
  BOOLEAN ProbeRttMinExpired;
  TimePoint ProbeRttMinTimeStamp;
  TimeDiff ProbeRttMin;

  // Other variables
  TimePoint ProbeRttDoneStamp; // Timestamp when the probe RTT phase is considered done.
  BOOLEAN LossRoundStart; // Identifying if we were in a loss round, set to true based on latest ACK event.
  Bytes LossRoundDelivered; // The amount of data delivered until current loss round

  Bytes NextRoundDelivered;
  BOOLEAN RoundStart;
  uint64_t RoundCount;
  uint64_t RoundsSinceBandwidthProbe;

  BOOLEAN LossInRound; // Whether loss has been noted in the current round.

  TimePoint CycleStamp;

  ACK_PHASE AckPhase; // The current ACK phase.

  TimeDiff BandwidthProbeWait; // The wait time for the next bandwidth probe phase.

  uint64_t ProbeUpCount;
  uint64_t BandwidthProbeUpRounds;
  uint64_t BandwidthProbeUpAcks;

  BOOLEAN BandwidthProbeSamples; // Whether we have samples of BW_PROBE, set after first round of BW_REFILL

  BOOLEAN ProbeRttRoundDone;

  TimePoint(*now)(void); // Function pointer to get the current time.

} QUIC_CONGESTION_CONTROL_BBRV3;


void BbrV3CongestionControlInitialize(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_opt_ const QUIC_SETTINGS_INTERNAL* Settings
);

void BbrV3CongestionControlInitializeImpl(
    _In_ QUIC_CONGESTION_CONTROL* Cc,
    _In_opt_ const QUIC_SETTINGS_INTERNAL* Settings,
    _In_ TimePoint (*now)(void)
);

TimePoint BbrV3NowUs(void);
