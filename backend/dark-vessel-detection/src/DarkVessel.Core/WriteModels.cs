namespace DarkVessel.Core;

public sealed record PositionWrite(long Mmsi, DateTime Ts, double Lat, double Lon, double? Sog, double? Cog, double? Heading, int? NavStatus);

public sealed record VesselWrite(long Mmsi, string? Name, string? Callsign, long? Imo, int? ShipType, double? LengthM, double? WidthM, DateTime SeenAt);

public sealed record ArchiveStats(long? RowsEstimate, long? Bytes, DateTime? Oldest, DateTime? Newest);