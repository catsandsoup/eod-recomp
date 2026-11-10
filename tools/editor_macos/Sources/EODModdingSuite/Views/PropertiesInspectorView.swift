import SwiftUI

public struct PropertiesInspectorView: View {
    @ObservedObject var state: AppState
    @State private var selectedTab: String = "Object"

    private let tabs = ["Object", "Physics", "Damage", "Props"]

    public init(state: AppState) {
        self.state = state
    }

    public var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                Image(systemName: "slider.horizontal.3")
                    .foregroundColor(.blue)
                    .font(.system(size: 11))
                Text("Properties")
                    .font(.system(size: 12, weight: .bold))
                Spacer()
                Text(state.vehicle.name)
                    .font(.system(size: 10, weight: .medium))
                    .foregroundColor(.secondary)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .background(Color(red: 0.14, green: 0.15, blue: 0.18))

            Divider().opacity(0.3)

            // Sub tabs
            HStack(spacing: 4) {
                ForEach(tabs, id: \.self) { tab in
                    Button(action: { selectedTab = tab }) {
                        Text(tab)
                            .font(.system(size: 10, weight: selectedTab == tab ? .bold : .medium))
                            .padding(.horizontal, 8)
                            .padding(.vertical, 3)
                            .background(selectedTab == tab ? Color.blue.opacity(0.8) : Color.white.opacity(0.05))
                            .foregroundColor(selectedTab == tab ? .white : .secondary)
                            .cornerRadius(4)
                    }
                    .buttonStyle(.plain)
                }
                Spacer()
            }
            .padding(6)

            Divider().opacity(0.3)

            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    // Vehicle mini preview card
                    ZStack {
                        RoundedRectangle(cornerRadius: 6)
                            .fill(Color(red: 0.18, green: 0.20, blue: 0.24))
                            .frame(height: 90)

                        VStack(spacing: 4) {
                            Image(systemName: "car.side.fill")
                                .font(.system(size: 38))
                                .foregroundColor(.red)
                            Text(state.vehicle.name)
                                .font(.system(size: 11, weight: .bold))
                                .foregroundColor(.white)
                        }
                    }
                    .frame(height: 90)
                    .padding(.horizontal, 8)
                    .padding(.top, 6)

                    if selectedTab == "Object" || selectedTab == "Props" {
                        // General Info Section
                        inspectorSection("Identity") {
                            inspectorField("Name", value: $state.vehicle.name)
                            inspectorField("Type", value: $state.vehicle.type)
                            inspectorField("Class", value: $state.vehicle.carClass)
                            inspectorField("Manufacturer", value: $state.vehicle.manufacturer)
                            
                            HStack {
                                Text("Year")
                                    .font(.system(size: 11))
                                    .foregroundColor(.secondary)
                                    .frame(width: 85, alignment: .leading)
                                TextField("Year", value: $state.vehicle.year, format: .number)
                                    .textFieldStyle(.roundedBorder)
                                    .font(.system(size: 11))
                            }
                        }
                    }

                    if selectedTab == "Physics" || selectedTab == "Object" {
                        // Physics Section
                        inspectorSection("Physics") {
                            HStack {
                                Text("Mass (kg)")
                                    .font(.system(size: 11))
                                    .foregroundColor(.secondary)
                                    .frame(width: 85, alignment: .leading)
                                TextField("Mass", value: $state.vehicle.massKg, format: .number)
                                    .textFieldStyle(.roundedBorder)
                                    .font(.system(size: 11))
                            }

                            HStack {
                                Text("Top Speed")
                                    .font(.system(size: 11))
                                    .foregroundColor(.secondary)
                                    .frame(width: 85, alignment: .leading)
                                TextField("Speed", value: $state.vehicle.topSpeedKmh, format: .number)
                                    .textFieldStyle(.roundedBorder)
                                    .font(.system(size: 11))
                            }

                            HStack {
                                Text("Grip")
                                    .font(.system(size: 11))
                                    .foregroundColor(.secondary)
                                    .frame(width: 85, alignment: .leading)
                                TextField("Grip", value: $state.vehicle.grip, format: .number)
                                    .textFieldStyle(.roundedBorder)
                                    .font(.system(size: 11))
                            }

                            VStack(alignment: .leading, spacing: 2) {
                                HStack {
                                    Text("Handling")
                                        .font(.system(size: 11))
                                        .foregroundColor(.secondary)
                                    Spacer()
                                    Text(String(format: "%.0f%%", state.vehicle.handlingPercent))
                                        .font(.system(size: 11, weight: .bold))
                                }
                                Slider(value: $state.vehicle.handlingPercent, in: 0...100)
                            }
                        }
                    }

                    if selectedTab == "Damage" || selectedTab == "Object" {
                        // Damage Section
                        inspectorSection("Damage & Integrity") {
                            VStack(alignment: .leading, spacing: 2) {
                                HStack {
                                    Text("Health")
                                        .font(.system(size: 11))
                                        .foregroundColor(.secondary)
                                    Spacer()
                                    Text(String(format: "%.0f%%", state.vehicle.healthPercent))
                                        .font(.system(size: 11, weight: .bold))
                                }
                                Slider(value: $state.vehicle.healthPercent, in: 0...100)
                            }

                            Toggle("Detachable Parts", isOn: $state.vehicle.detachableParts)
                                .font(.system(size: 11))
                                .toggleStyle(.switch)
                                .controlSize(.small)

                            VStack(alignment: .leading, spacing: 2) {
                                HStack {
                                    Text("Deformation Preview")
                                        .font(.system(size: 11))
                                        .foregroundColor(.secondary)
                                    Spacer()
                                    Text(String(format: "%.0f%%", state.vehicle.deformationAmount * 100))
                                        .font(.system(size: 11, weight: .bold))
                                }
                                Slider(value: $state.vehicle.deformationAmount, in: 0.0...1.0)
                            }
                        }
                    }

                    if selectedTab == "Props" {
                        // Track & Scenery Prop Rules
                        inspectorSection("Prop Collision Rules") {
                            Toggle("Indestructible", isOn: $state.vehicle.indestructibleProps)
                                .font(.system(size: 11))
                                .toggleStyle(.switch)
                                .controlSize(.small)

                            Toggle("Immovable", isOn: $state.vehicle.immovableProps)
                                .font(.system(size: 11))
                                .toggleStyle(.switch)
                                .controlSize(.small)

                            HStack {
                                Text("Height Limit (m)")
                                    .font(.system(size: 11))
                                    .foregroundColor(.secondary)
                                    .frame(width: 100, alignment: .leading)
                                TextField("Height Limit", value: $state.vehicle.heightLimitM, format: .number)
                                    .textFieldStyle(.roundedBorder)
                                    .font(.system(size: 11))
                            }
                        }
                    }
                }
                .padding(8)
            }
        }
        .background(Color(red: 0.13, green: 0.14, blue: 0.16))
    }

    private func inspectorSection<Content: View>(_ title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title.uppercased())
                .font(.system(size: 10, weight: .bold))
                .foregroundColor(.secondary)
            
            VStack(alignment: .leading, spacing: 8) {
                content()
            }
            .padding(8)
            .background(Color.white.opacity(0.03))
            .cornerRadius(6)
        }
    }

    private func inspectorField(_ label: String, value: Binding<String>) -> some View {
        HStack {
            Text(label)
                .font(.system(size: 11))
                .foregroundColor(.secondary)
                .frame(width: 85, alignment: .leading)
            TextField(label, text: value)
                .textFieldStyle(.roundedBorder)
                .font(.system(size: 11))
        }
    }
}
