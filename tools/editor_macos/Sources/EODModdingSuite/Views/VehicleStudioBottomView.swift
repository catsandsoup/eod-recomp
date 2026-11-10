import SwiftUI

public struct VehicleStudioBottomView: View {
    @ObservedObject var state: AppState
    @State private var subTab: String = "Components"
    
    // The 32 standard component groups in EOD vehicle deformation architecture
    @State private var componentStates: [String: Bool] = [
        "Body Panels": true,
        "Front Left Door": true,
        "Front Right Door": true,
        "Rear Left Door": false,
        "Rear Right Door": false,
        "Front Bumper": true,
        "Rear Bumper": true,
        "Engine Hood": true,
        "Trunk Lid": true,
        "Left Front Fender": true,
        "Right Front Fender": true,
        "Left Rear Quarter": true,
        "Right Rear Quarter": true,
        "Front Windshield": true,
        "Rear Window": true,
        "Roof Panel": true,
        "Front Grille": true,
        "Engine Block & Radiator": true,
        "Roll Cage": true,
        "Front Left Wheel": true,
        "Front Right Wheel": true,
        "Rear Left Wheel": true,
        "Rear Right Wheel": true,
        "Steering Column": true,
        "Rear Spoiler": true,
        "Dual Exhaust Pipes": true,
        "Suspension A-Arms": true,
        "Driveshaft": true,
        "Fuel Cell": true,
        "Battery Box": true,
        "Headlight Assemblies": true,
        "Tail Light Clusters": true
    ]

    public init(state: AppState) {
        self.state = state
    }

    public var body: some View {
        HStack(spacing: 0) {
            // Left: Exploded Parts / Deformation Wireframe View
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("Vehicle Studio — \(state.vehicle.name) Setup")
                        .font(.system(size: 11, weight: .bold))

                    Spacer()

                    // Sub tabs
                    HStack(spacing: 2) {
                        ForEach(["Overview", "Components", "Sockets", "Deformation"], id: \.self) { tab in
                            Button(action: { subTab = tab }) {
                                Text(tab)
                                    .font(.system(size: 10, weight: subTab == tab ? .bold : .medium))
                                    .padding(.horizontal, 6)
                                    .padding(.vertical, 2)
                                    .background(subTab == tab ? Color.blue.opacity(0.8) : Color.white.opacity(0.05))
                                    .foregroundColor(subTab == tab ? .white : .secondary)
                                    .cornerRadius(3)
                            }
                            .buttonStyle(.plain)
                        }
                    }
                }
                .padding(.horizontal, 10)
                .padding(.top, 6)

                // Wireframe / Exploded schematic visualization
                ZStack {
                    RoundedRectangle(cornerRadius: 8)
                        .fill(Color(red: 0.10, green: 0.11, blue: 0.13))

                    VStack(spacing: 8) {
                        HStack(spacing: 16) {
                            partIconCard("Hood", icon: "car.side.fill", color: .red)
                            partIconCard("Doors", icon: "door.left.hand.open", color: .orange)
                            partIconCard("Bumpers", icon: "shield.lefthalf.filled", color: .yellow)
                            partIconCard("Wheels", icon: "circle.circle.fill", color: .cyan)
                            partIconCard("Spoiler", icon: "airplane", color: .purple)
                        }

                        // Deformation slider indicator
                        HStack(spacing: 10) {
                            Text("Crumple Simulation:")
                                .font(.system(size: 10))
                                .foregroundColor(.secondary)
                            Slider(value: $state.vehicle.deformationAmount, in: 0...1)
                                .frame(width: 180)
                            Text(String(format: "%.0f%% Wrecked", state.vehicle.deformationAmount * 100))
                                .font(.system(size: 10, weight: .bold, design: .monospaced))
                                .foregroundColor(state.vehicle.deformationAmount > 0.5 ? .red : .green)
                        }
                        .padding(.horizontal, 12)
                        .padding(.vertical, 4)
                        .background(Color.black.opacity(0.4))
                        .cornerRadius(6)
                    }
                }
                .padding(6)
            }
            .frame(maxWidth: .infinity)

            Divider().opacity(0.3)

            // Right: 32 Component Groups Checklist
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text("32 Component Groups")
                        .font(.system(size: 11, weight: .bold))
                    Spacer()
                    let activeCount = componentStates.values.filter { $0 }.count
                    Text("\(activeCount)/32 Active")
                        .font(.system(size: 9, weight: .bold))
                        .foregroundColor(.blue)
                }
                .padding(.top, 6)

                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 4) {
                        ForEach(componentStates.keys.sorted(), id: \.self) { component in
                            Toggle(isOn: Binding(
                                get: { componentStates[component] ?? false },
                                set: { componentStates[component] = $0 }
                            )) {
                                Text(component)
                                    .font(.system(size: 10))
                            }
                            .toggleStyle(.checkbox)
                        }
                    }
                }

                Divider().opacity(0.3)

                HStack {
                    Button("Select All") {
                        for k in componentStates.keys { componentStates[k] = true }
                    }
                    .font(.system(size: 9))
                    .buttonStyle(.plain)

                    Spacer()

                    Button("Clear Non-Essential") {
                        for k in componentStates.keys {
                            if k.contains("Spoiler") || k.contains("Bumper") {
                                componentStates[k] = false
                            }
                        }
                    }
                    .font(.system(size: 9))
                    .foregroundColor(.secondary)
                    .buttonStyle(.plain)
                }
            }
            .frame(width: 220)
            .padding(.horizontal, 10)
            .padding(.bottom, 8)
            .background(Color(red: 0.13, green: 0.14, blue: 0.16))
        }
        .background(Color(red: 0.11, green: 0.12, blue: 0.14))
    }

    private func partIconCard(_ name: String, icon: String, color: Color) -> some View {
        VStack(spacing: 3) {
            Circle()
                .fill(color.opacity(0.2))
                .frame(width: 36, height: 36)
                .overlay(
                    Image(systemName: icon)
                        .font(.system(size: 16))
                        .foregroundColor(color)
                )
            Text(name)
                .font(.system(size: 9, weight: .medium))
                .foregroundColor(.secondary)
        }
        .padding(6)
        .background(RoundedRectangle(cornerRadius: 6).fill(Color.white.opacity(0.04)))
    }
}
