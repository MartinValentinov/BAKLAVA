namespace DarkVessel.Core;

/// <summary>One AIS position to persist. Shared shape between whichever backend
/// actually writes it (direct MySQL via AisStore, or an HTTP API via HttpAisSource).</summary>
public sealed record PositionWrite(long Mmsi, DateTime Ts, double Lat, double Lon, double? Sog, double? Cog, double? Heading, int? NavStatus);

/// <summary>One vessel's static data to upsert. Same sharing rationale as <see cref="PositionWrite"/>.</summary>
public sealed record VesselWrite(long Mmsi, string? Name, string? Callsign, long? Imo, int? ShipType, double? LengthM, double? WidthM, DateTime SeenAt);

/// <summary>Archive size/span, for a dashboard. Same sharing rationale as <see cref="PositionWrite"/>.</summary>
public sealed record ArchiveStats(long? RowsEstimate, long? Bytes, DateTime? Oldest, DateTime? Newest);