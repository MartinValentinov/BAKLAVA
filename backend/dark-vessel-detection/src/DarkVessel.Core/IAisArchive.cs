namespace DarkVessel.Core;

public interface IAisArchive : IAisSource
{
    Task<long> OpenCoverageAsync(string bboxesJson, string host, CancellationToken ct = default);

    Task TouchCoverageAsync(long id, int messages, DateTime? lastMessageAt, CancellationToken ct = default);

    Task CloseCoverageAsync(long id, int messages, CancellationToken ct = default);

    Task<int> InsertPositionsAsync(IReadOnlyList<PositionWrite> rows, CancellationToken ct = default);

    Task UpsertVesselsAsync(IReadOnlyList<VesselWrite> rows, CancellationToken ct = default);

    Task<ArchiveStats> ArchiveStatsAsync(CancellationToken ct = default);
}
