import SwiftUI

public struct EventEditorBottomView: View {
    @ObservedObject var state: AppState
    @State private var timeLimit: String = "3:00"
    @State private var spawnCount: Int = 12
    @State private var aggression: String = "High"
    @State private var arena: String = "Stadium Derby"

    public init(state: AppState) {
        self.state = state
    }

    public var body: some View {
        HStack(spacing: 0) {
            // Left: Visual Event Node Flowchart
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("Event Editor — Demolition Derby Logic Graph")
                        .font(.system(size: 11, weight: .bold))
                    Spacer()
                    Button("+ Add Event Node") {}
                        .font(.system(size: 10))
                        .buttonStyle(.plain)
                        .foregroundColor(.blue)
                }
                .padding(.horizontal, 10)
                .padding(.top, 6)

                // Visual Flowchart Diagram
                ZStack {
                    RoundedRectangle(cornerRadius: 8)
                        .fill(Color(red: 0.10, green: 0.11, blue: 0.13))

                    HStack(spacing: 16) {
                        eventNode(title: "Start Event", subtitle: "On Countdown 0", color: .green)
                        arrowConnector()
                        eventNode(title: "Spawn Vehicles", subtitle: "\(spawnCount) Cars Random Grid", color: .blue)
                        arrowConnector()
                        eventNode(title: "Set Rules", subtitle: "No Respawn • Point Derby", color: .purple)
                        arrowConnector()
                        eventNode(title: "End on Last Car", subtitle: "Declare Winner", color: .orange)
                    }
                }
                .padding(6)
            }
            .frame(maxWidth: .infinity)

            Divider().opacity(0.3)

            // Right: Event Configuration
            VStack(alignment: .leading, spacing: 8) {
                Text("Event Properties")
                    .font(.system(size: 11, weight: .bold))
                    .padding(.top, 6)

                HStack {
                    Text("Time Limit")
                        .font(.system(size: 10))
                        .foregroundColor(.secondary)
                    Spacer()
                    TextField("3:00", text: $timeLimit)
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 65)
                        .font(.system(size: 10))
                }

                HStack {
                    Text("Spawn Count")
                        .font(.system(size: 10))
                        .foregroundColor(.secondary)
                    Spacer()
                    Stepper("\(spawnCount) cars", value: $spawnCount, in: 2...24)
                        .font(.system(size: 10))
                }

                HStack {
                    Text("Aggression")
                        .font(.system(size: 10))
                        .foregroundColor(.secondary)
                    Spacer()
                    Picker("", selection: $aggression) {
                        Text("Low").tag("Low")
                        Text("Medium").tag("Medium")
                        Text("High").tag("High")
                        Text("Brutal").tag("Brutal")
                    }
                    .labelsHidden()
                    .pickerStyle(.menu)
                }

                HStack {
                    Text("Arena")
                        .font(.system(size: 10))
                        .foregroundColor(.secondary)
                    Spacer()
                    Picker("", selection: $arena) {
                        Text("Stadium Derby").tag("Stadium Derby")
                        Text("Figure 8 Arena").tag("Figure 8 Arena")
                        Text("Dirt Oval").tag("Dirt Oval")
                    }
                    .labelsHidden()
                    .pickerStyle(.menu)
                }

                Spacer()

                Button("Export Event Manifest") {
                    state.buildStatus = "Exported event.manifest.json"
                }
                .font(.system(size: 10))
                .foregroundColor(.blue)
                .buttonStyle(.plain)
            }
            .frame(width: 200)
            .padding(.horizontal, 10)
            .padding(.bottom, 8)
            .background(Color(red: 0.13, green: 0.14, blue: 0.16))
        }
        .background(Color(red: 0.11, green: 0.12, blue: 0.14))
    }

    private func eventNode(title: String, subtitle: String, color: Color) -> some View {
        VStack(spacing: 3) {
            Text(title)
                .font(.system(size: 10, weight: .bold))
                .foregroundColor(.white)
            Text(subtitle)
                .font(.system(size: 8))
                .foregroundColor(.secondary)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(
            RoundedRectangle(cornerRadius: 6)
                .fill(color.opacity(0.25))
                .overlay(
                    RoundedRectangle(cornerRadius: 6)
                        .stroke(color, lineWidth: 1)
                )
        )
    }

    private func arrowConnector() -> some View {
        Image(systemName: "arrow.right")
            .font(.system(size: 11, weight: .bold))
            .foregroundColor(.secondary)
    }
}
