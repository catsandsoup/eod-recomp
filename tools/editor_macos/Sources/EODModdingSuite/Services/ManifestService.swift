import Foundation

@MainActor
public final class ManifestService {
    public static let shared = ManifestService()

    private init() {}

    public func exportVehicleManifest(vehicle: VehicleData, destinationDir: URL) throws -> URL {
        let fileURL = destinationDir.appendingPathComponent("vehicle.manifest.json")
        
        let manifestDict: [String: Any] = [
            "schemaVersion": 1,
            "metadata": [
                "name": vehicle.name,
                "type": vehicle.type,
                "class": vehicle.carClass,
                "manufacturer": vehicle.manufacturer,
                "year": vehicle.year
            ],
            "physics": [
                "massKg": vehicle.massKg,
                "topSpeedKmh": vehicle.topSpeedKmh,
                "grip": vehicle.grip,
                "handling": vehicle.handlingPercent / 100.0
            ],
            "damage": [
                "initialHealth": vehicle.healthPercent / 100.0,
                "detachableParts": vehicle.detachableParts,
                "deformationRatio": vehicle.deformationAmount
            ],
            "components": [
                ["name": "Body Panels", "active": vehicle.hasBodyPanels, "detachable": false],
                ["name": "Front Bumper", "active": vehicle.hasBumpers, "detachable": vehicle.detachableParts],
                ["name": "Rear Bumper", "active": vehicle.hasBumpers, "detachable": vehicle.detachableParts],
                ["name": "Engine Hood", "active": vehicle.hasHood, "detachable": vehicle.detachableParts],
                ["name": "Trunk Lid", "active": vehicle.hasTrunk, "detachable": vehicle.detachableParts],
                ["name": "Doors", "active": vehicle.hasDoors, "detachable": vehicle.detachableParts],
                ["name": "Wheels", "active": vehicle.hasWheels, "detachable": false],
                ["name": "Rear Spoiler", "active": vehicle.hasSpoiler, "detachable": vehicle.detachableParts]
            ]
        ]

        let data = try JSONSerialization.data(withJSONObject: manifestDict, options: [.prettyPrinted, .sortedKeys])
        try FileManager.default.createDirectory(at: destinationDir, withIntermediateDirectories: true)
        try data.write(to: fileURL)
        return fileURL
    }

    public func exportTrackManifest(track: TrackData, waypoints: [AIWaypoint], destinationDir: URL) throws -> URL {
        let fileURL = destinationDir.appendingPathComponent("track.manifest.json")

        let waypointList = waypoints.map { wp in
            [
                "id": wp.id,
                "position": ["x": wp.position.x, "y": 0.0, "z": wp.position.y],
                "speedLimitKmh": wp.speedLimitKmh,
                "isBranch": wp.isBranch
            ] as [String: Any]
        }

        let manifestDict: [String: Any] = [
            "schemaVersion": 1,
            "metadata": [
                "name": track.name,
                "type": track.type,
                "terrainSizeMeters": track.terrainSize,
                "heightLimitMeters": track.heightLimit
            ],
            "rules": [
                "spawnCount": track.spawnCount,
                "timeLimitMinutes": track.timeLimitMinutes,
                "allowExplosions": track.allowExplosions,
                "destructibleProps": track.destructibleProps,
                "randomizeSpawn": track.randomizeSpawn
            ],
            "ai": [
                "aggression": track.aiAggression / 100.0,
                "targetingMode": track.aiTargeting,
                "waypoints": waypointList
            ],
            "propRules": [
                "indestructibleDefault": false,
                "immovableDefault": false,
                "heightLimit": track.heightLimit
            ]
        ]

        let data = try JSONSerialization.data(withJSONObject: manifestDict, options: [.prettyPrinted, .sortedKeys])
        try FileManager.default.createDirectory(at: destinationDir, withIntermediateDirectories: true)
        try data.write(to: fileURL)
        return fileURL
    }

    public func validateManifestFile(url: URL) -> (Bool, String) {
        guard FileManager.default.fileExists(atPath: url.path) else {
            return (false, "File not found at \(url.path)")
        }
        do {
            let data = try Data(contentsOf: url)
            guard let json = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                return (false, "Manifest is not a valid JSON object")
            }
            guard let schemaVersion = json["schemaVersion"] as? Int, schemaVersion == 1 else {
                return (false, "Missing or unsupported schemaVersion (expected 1)")
            }
            guard json["metadata"] != nil else {
                return (false, "Missing required 'metadata' section")
            }
            return (true, "Valid schemaVersion 1 manifest (\(data.count) bytes)")
        } catch {
            return (false, "JSON parsing error: \(error.localizedDescription)")
        }
    }
}
