import SwiftUI

public struct AssetBrowserView: View {
    @ObservedObject var state: AppState
    @State private var selectedCategory: String = "Vehicles"
    @State private var filterType: String = "All"
    
    private let categories = [
        ("Base Game", "archivebox.fill", [
            ("Vehicles", "car.2.fill"),
            ("Tracks", "road.lanes"),
            ("Props", "shippingbox.fill"),
            ("Audio", "speaker.wave.3.fill")
        ]),
        ("My Mod", "folder.badge.gearshape", [
            ("Vehicles", "car.fill"),
            ("Tracks", "flag.checkered"),
            ("Props", "cube.fill"),
            ("Audio", "waveform")
        ])
    ]

    private let filterTabs = ["All", "Models", "Textures", "Audio", "Prefabs"]

    public init(state: AppState) {
        self.state = state
    }

    public var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                Image(systemName: "folder.fill")
                    .foregroundColor(.blue)
                    .font(.system(size: 11))
                Text("Asset Browser")
                    .font(.system(size: 12, weight: .bold))
                Spacer()
                Button(action: {}) {
                    Image(systemName: "plus")
                        .font(.system(size: 11))
                }
                .buttonStyle(.plain)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .background(Color(red: 0.14, green: 0.15, blue: 0.18))

            Divider().opacity(0.3)

            // Search Bar
            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass")
                    .foregroundColor(.secondary)
                    .font(.system(size: 11))
                TextField("Search assets...", text: $state.searchQuery)
                    .textFieldStyle(.plain)
                    .font(.system(size: 11))
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 5)
            .background(Color.black.opacity(0.25))
            .cornerRadius(5)
            .padding(6)

            // Filter Tabs
            HStack(spacing: 4) {
                ForEach(filterTabs, id: \.self) { tab in
                    Button(action: { filterType = tab }) {
                        Text(tab)
                            .font(.system(size: 10, weight: filterType == tab ? .bold : .medium))
                            .padding(.horizontal, 7)
                            .padding(.vertical, 3)
                            .background(
                                filterType == tab ? Color.blue.opacity(0.8) : Color.white.opacity(0.05)
                            )
                            .foregroundColor(filterType == tab ? .white : .secondary)
                            .cornerRadius(4)
                    }
                    .buttonStyle(.plain)
                }
                Spacer()
            }
            .padding(.horizontal, 6)
            .padding(.bottom, 6)

            Divider().opacity(0.3)

            // Master-Detail Split: Category Tree on Left, Assets Grid on Right
            HSplitView {
                // Folder hierarchy
                List {
                    Section("Projects") {
                        ForEach(categories, id: \.0) { group, icon, items in
                            DisclosureGroup(
                                isExpanded: .constant(true),
                                content: {
                                    ForEach(items, id: \.0) { item, itemIcon in
                                        HStack(spacing: 6) {
                                            Image(systemName: itemIcon)
                                                .foregroundColor(selectedCategory == item ? .blue : .secondary)
                                                .font(.system(size: 11))
                                            Text(item)
                                                .font(.system(size: 11))
                                            Spacer()
                                        }
                                        .padding(.vertical, 2)
                                        .contentShape(Rectangle())
                                        .onTapGesture {
                                            selectedCategory = item
                                        }
                                        .listRowBackground(
                                            selectedCategory == item ? Color.blue.opacity(0.2) : Color.clear
                                        )
                                    }
                                },
                                label: {
                                    HStack(spacing: 6) {
                                        Image(systemName: icon)
                                            .foregroundColor(.yellow)
                                            .font(.system(size: 11))
                                        Text(group)
                                            .font(.system(size: 11, weight: .semibold))
                                    }
                                }
                            )
                        }
                    }
                }
                .listStyle(.sidebar)
                .frame(minWidth: 130, idealWidth: 150, maxWidth: 200)

                // Asset Cards Grid
                ScrollView {
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 120, maximum: 160), spacing: 8)], spacing: 8) {
                        ForEach(filteredAssets) { asset in
                            AssetCard(asset: asset, isSelected: state.selectedAssetId == asset.id)
                                .onTapGesture {
                                    state.selectedAssetId = asset.id
                                    if asset.category == "Vehicles" {
                                        state.vehicle.name = asset.name
                                    }
                                }
                        }
                    }
                    .padding(8)
                }
                .background(Color(red: 0.11, green: 0.12, blue: 0.14))
            }
        }
        .background(Color(red: 0.13, green: 0.14, blue: 0.16))
    }

    private var filteredAssets: [AssetItem] {
        state.assets.filter { asset in
            let matchesCategory = (selectedCategory == "All" || asset.category == selectedCategory)
            let matchesSearch = state.searchQuery.isEmpty || asset.name.localizedCaseInsensitiveContains(state.searchQuery)
            return matchesCategory && matchesSearch
        }
    }
}

private struct AssetCard: View {
    let asset: AssetItem
    let isSelected: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            ZStack {
                RoundedRectangle(cornerRadius: 6)
                    .fill(Color(red: 0.18, green: 0.20, blue: 0.24))
                    .frame(height: 80)
                
                Image(systemName: iconForCategory(asset.category))
                    .font(.system(size: 32))
                    .foregroundColor(asset.color.opacity(0.85))

                VStack {
                    Spacer()
                    HStack {
                        Spacer()
                        Text(asset.subCategory)
                            .font(.system(size: 8, weight: .bold))
                            .padding(.horizontal, 4)
                            .padding(.vertical, 2)
                            .background(Color.black.opacity(0.6))
                            .cornerRadius(3)
                            .foregroundColor(.white)
                    }
                    .padding(4)
                }
            }
            .frame(height: 80)

            Text(asset.name)
                .font(.system(size: 11, weight: .semibold))
                .lineLimit(1)
                .foregroundColor(.white)

            Text(asset.detail)
                .font(.system(size: 9))
                .lineLimit(1)
                .foregroundColor(.secondary)

            // Tags
            HStack(spacing: 3) {
                ForEach(asset.tags.prefix(2), id: \.self) { tag in
                    Text(tag)
                        .font(.system(size: 8))
                        .padding(.horizontal, 4)
                        .padding(.vertical, 1)
                        .background(Color.white.opacity(0.08))
                        .cornerRadius(2)
                        .foregroundColor(.secondary)
                }
            }
        }
        .padding(6)
        .background(
            RoundedRectangle(cornerRadius: 8)
                .fill(isSelected ? Color.blue.opacity(0.2) : Color.white.opacity(0.03))
                .overlay(
                    RoundedRectangle(cornerRadius: 8)
                        .stroke(isSelected ? Color.blue : Color.white.opacity(0.08), lineWidth: 1)
                )
        )
    }

    private func iconForCategory(_ category: String) -> String {
        switch category {
        case "Vehicles": return "car.side.fill"
        case "Tracks": return "road.lanes.curved.right"
        case "Scenery": return "building.2.crop.list"
        default: return "cube.fill"
        }
    }
}
