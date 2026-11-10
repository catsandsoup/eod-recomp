import Foundation
import SwiftUI
import Combine

@MainActor
public final class AppState: ObservableObject {
    @Published public var activeStudio: StudioTab = .worldStudio
    @Published public var activeBottomTab: String = "Vehicle Studio"
    @Published public var selectedGizmo: String = "Move"
    @Published public var isCoordinateGlobal: Bool = true
    @Published public var cameraSpeed: Double = 1.0
    
    // Stats
    @Published public var fps: Int = 144
    @Published public var drawCalls: Int = 2847
    @Published public var trisCount: String = "1.9M"
    @Published public var memoryUsage: String = "3.2 GB"
    @Published public var buildStatus: String = "Build Ready"
    @Published public var projectName: String = "MyEODMod"
    @Published public var projectPath: String = "/Users/USER/Documents/GitHub/eod-recomp/mods/MyEODMod"
    
    // Core Data
    @Published public var vehicle: VehicleData = VehicleData()
    @Published public var track: TrackData = TrackData()
    @Published public var selectedAssetCategory: String = "All"
    @Published public var searchQuery: String = ""
    
    // Assets
    @Published public var assets: [AssetItem] = [
        AssetItem(id: "phantom_gt", name: "Phantom GT", category: "Vehicles", subCategory: "Custom", detail: "Midsize • 1974 • 280 km/h", tags: ["Car", "Custom", "Classic"], color: .red),
        AssetItem(id: "muscle_1", name: "Razor Coupe", category: "Vehicles", subCategory: "Custom", detail: "Muscle • 1969 • 290 km/h", tags: ["Car", "Muscle", "V8"], color: .orange),
        AssetItem(id: "racer_pro", name: "Destruction Truck", category: "Vehicles", subCategory: "Custom", detail: "Heavy • 1980 • 210 km/h", tags: ["Truck", "Heavy", "Derby"], color: .blue),
        AssetItem(id: "dirt_king", name: "Custom Ute", category: "Vehicles", subCategory: "Custom", detail: "Light • 1978 • 240 km/h", tags: ["Ute", "Light"], color: .yellow),
        AssetItem(id: "stadium_derby", name: "Stadium Derby", category: "Tracks", subCategory: "Derby", detail: "Dirt Arena • 500m • 16 Cars", tags: ["Arena", "Dirt", "Derby"], color: .purple),
        AssetItem(id: "coastal_run", name: "Coastal Run", category: "Tracks", subCategory: "Circuit", detail: "Asphalt & Dirt • 2.4 km", tags: ["Circuit", "Long"], color: .green),
        AssetItem(id: "concrete_barrier", name: "Building_Barrier_01", category: "Scenery", subCategory: "Props", detail: "Concrete Wall • Indestructible", tags: ["Barrier", "Heavy"], color: .gray),
        AssetItem(id: "dirt_mound", name: "Stadium Ramp", category: "Scenery", subCategory: "Props", detail: "Stunt Ramp • Wood/Steel", tags: ["Ramp", "Stunt"], color: .brown)
    ]
    @Published public var selectedAssetId: String = "phantom_gt"
    
    // Scene Hierarchy
    @Published public var sceneRoot: SceneNode = SceneNode(name: "EOD_Map_001", icon: "cube.transparent", children: [
        SceneNode(name: "Terrain", icon: "mountain.2.fill"),
        SceneNode(name: "Track", icon: "road.lanes", children: [
            SceneNode(name: "Track Mesh", icon: "mesh"),
            SceneNode(name: "Collision", icon: "shield.fill"),
            SceneNode(name: "Spline", icon: "arrow.triangle.turn.up.right.diamond")
        ]),
        SceneNode(name: "Scenery", icon: "building.2.fill", children: [
            SceneNode(name: "Stadium", icon: "building.fill"),
            SceneNode(name: "Billboards", icon: "rectangle.fill"),
            SceneNode(name: "Barriers", icon: "square.fill"),
            SceneNode(name: "Trees", icon: "leaf.fill")
        ]),
        SceneNode(name: "Spawn Points", icon: "location.fill", children: [
            SceneNode(name: "Player Spawn", icon: "person.fill"),
            SceneNode(name: "AI Spawns (15)", icon: "person.3.fill")
        ]),
        SceneNode(name: "Events", icon: "flag.2.crossed.fill", children: [
            SceneNode(name: "Demolition Derby 01", icon: "burst.fill")
        ]),
        SceneNode(name: "Lighting", icon: "sun.max.fill", children: [
            SceneNode(name: "Sun Light", icon: "sun.max"),
            SceneNode(name: "Skybox", icon: "cloud.sun.fill"),
            SceneNode(name: "Ambient", icon: "lightbulb.fill")
        ])
    ])
    
    // AI Waypoints
    @Published public var waypoints: [AIWaypoint] = [
        AIWaypoint(id: 0, position: CGPoint(x: 100, y: 340), isBranch: true),
        AIWaypoint(id: 1, position: CGPoint(x: 180, y: 380)),
        AIWaypoint(id: 2, position: CGPoint(x: 320, y: 380)),
        AIWaypoint(id: 3, position: CGPoint(x: 400, y: 340)),
        AIWaypoint(id: 4, position: CGPoint(x: 440, y: 250)),
        AIWaypoint(id: 5, position: CGPoint(x: 400, y: 160)),
        AIWaypoint(id: 6, position: CGPoint(x: 320, y: 120)),
        AIWaypoint(id: 7, position: CGPoint(x: 180, y: 120)),
        AIWaypoint(id: 8, position: CGPoint(x: 100, y: 160)),
        AIWaypoint(id: 9, position: CGPoint(x: 60, y: 250))
    ]
    @Published public var selectedWaypointId: Int? = 0
    
    public init() {}
    
    public func triggerBuild() {
        buildStatus = "Building manifests..."
        let destURL = URL(fileURLWithPath: projectPath)
        
        do {
            let vehicleURL = try ManifestService.shared.exportVehicleManifest(vehicle: vehicle, destinationDir: destURL)
            let trackURL = try ManifestService.shared.exportTrackManifest(track: track, waypoints: waypoints, destinationDir: destURL)
            
            let (vOk, vMsg) = ManifestService.shared.validateManifestFile(url: vehicleURL)
            let (tOk, tMsg) = ManifestService.shared.validateManifestFile(url: trackURL)
            
            if vOk && tOk {
                buildStatus = "Build Successful — Manifests Validated"
            } else {
                buildStatus = "Build Error: \(vOk ? tMsg : vMsg)"
            }
        } catch {
            buildStatus = "Build Failed: \(error.localizedDescription)"
        }
    }

    public func launchGame(headless: Bool = false, frames: Int? = nil) {
        buildStatus = "Launching Game Engine..."
        triggerBuild()
        
        EngineLauncherService.shared.launchGame(headless: headless, frames: frames) { [weak self] exitCode, output in
            guard let self = self else { return }
            if exitCode == 0 {
                self.buildStatus = "Game Process Exited Cleanly (0)"
            } else {
                self.buildStatus = "Game Process Error (\(exitCode))"
            }
        }
    }
}
