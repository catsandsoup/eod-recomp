import Foundation
import SwiftUI

public enum StudioTab: String, CaseIterable, Identifiable {
    case assetStudio = "Asset Studio"
    case worldStudio = "World Studio"
    case vehicleStudio = "Vehicle Studio"
    case aiStudio = "AI Studio"
    case eventStudio = "Event Studio"
    case uiStudio = "UI Studio"
    case audioStudio = "Audio Studio"
    case buildTest = "Build & Test"

    public var id: String { rawValue }

    public var icon: String {
        switch self {
        case .assetStudio: return "folder.fill"
        case .worldStudio: return "globe.americas.fill"
        case .vehicleStudio: return "car.fill"
        case .aiStudio: return "brain.head.profile"
        case .eventStudio: return "flag.checkered.2.crossed"
        case .uiStudio: return "macwindow"
        case .audioStudio: return "waveform"
        case .buildTest: return "play.circle.fill"
        }
    }
}

public struct AssetItem: Identifiable, Hashable {
    public let id: String
    public let name: String
    public let category: String
    public let subCategory: String
    public let detail: String
    public let tags: [String]
    public let color: Color

    public init(id: String, name: String, category: String, subCategory: String, detail: String, tags: [String] = [], color: Color = .blue) {
        self.id = id
        self.name = name
        self.category = category
        self.subCategory = subCategory
        self.detail = detail
        self.tags = tags
        self.color = color
    }
}

public struct SceneNode: Identifiable, Hashable {
    public let id = UUID()
    public var name: String
    public var icon: String
    public var children: [SceneNode]?
    public var isExpanded: Bool = true

    public init(name: String, icon: String, children: [SceneNode]? = nil) {
        self.name = name
        self.icon = icon
        self.children = children
    }
}

public struct VehicleData: Codable {
    public var name: String = "Phantom GT"
    public var type: String = "Vehicle"
    public var carClass: String = "Midsize"
    public var manufacturer: String = "Custom Works"
    public var year: Int = 1974
    public var massKg: Double = 1360.77
    public var topSpeedKmh: Double = 280.0
    public var grip: Double = 0.92
    public var handlingPercent: Double = 75.0
    public var healthPercent: Double = 100.0
    public var detachableParts: Bool = true
    public var indestructibleProps: Bool = false
    public var immovableProps: Bool = false
    public var heightLimitM: Double = 5.0
    
    // Components
    public var hasBodyPanels: Bool = true
    public var hasDoors: Bool = true
    public var hasBumpers: Bool = true
    public var hasHood: Bool = true
    public var hasTrunk: Bool = true
    public var hasWheels: Bool = true
    public var hasSpoiler: Bool = true
    
    // Deformation preview slider (0.0 = rest, 1.0 = wrecked)
    public var deformationAmount: Double = 0.0

    public init() {}
}

public struct TrackData: Codable {
    public var name: String = "Stadium_Derby_01"
    public var type: String = "Demolition Derby"
    public var terrainSize: Double = 500.0
    public var heightLimit: Double = 15.0
    public var frictionIndex: Int = 0 // Dirt
    public var spawnCount: Int = 16
    public var timeLimitMinutes: Double = 3.0
    public var allowExplosions: Bool = true
    public var destructibleProps: Bool = true
    public var randomizeSpawn: Bool = true
    public var aiAggression: Double = 75.0
    public var aiTargeting: String = "Most Damaged"

    public init() {}
}

public struct AIWaypoint: Identifiable {
    public let id: Int
    public var position: CGPoint
    public var isBranch: Bool
    public var speedLimitKmh: Double
    public var isSelected: Bool

    public init(id: Int, position: CGPoint, isBranch: Bool = false, speedLimitKmh: Double = 150.0, isSelected: Bool = false) {
        self.id = id
        self.position = position
        self.isBranch = isBranch
        self.speedLimitKmh = speedLimitKmh
        self.isSelected = isSelected
    }
}
