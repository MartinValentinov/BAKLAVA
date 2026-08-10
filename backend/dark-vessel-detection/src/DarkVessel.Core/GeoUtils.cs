namespace DarkVessel.Core;

public static class GeoUtils
{
    private const double EarthRadiusKm = 6371.0088;

    public static double HaversineKm(double lat1, double lon1, double lat2, double lon2)
    {
        double phi1 = DegreesToRadians(lat1);
        double phi2 = DegreesToRadians(lat2);
        double dPhi = DegreesToRadians(lat2 - lat1);
        double dLambda = DegreesToRadians(lon2 - lon1);

        double a = Math.Sin(dPhi / 2) * Math.Sin(dPhi / 2)
                   + Math.Cos(phi1) * Math.Cos(phi2) * Math.Sin(dLambda / 2) * Math.Sin(dLambda / 2);

        return 2 * EarthRadiusKm * Math.Asin(Math.Min(1.0, Math.Sqrt(a)));
    }

    private static double DegreesToRadians(double degrees) => degrees * Math.PI / 180.0;

    public static (double LatLo, double LatHi, double LonLo, double LonHi) BoundingBox(
        double lat, double lon, double radiusKm)
    {
        double dLat = radiusKm / 111.0;
        double dLon = radiusKm / Math.Max(111.0 * Math.Cos(DegreesToRadians(lat)), 1e-6);
        return (lat - dLat, lat + dLat, lon - dLon, lon + dLon);
    }
}
