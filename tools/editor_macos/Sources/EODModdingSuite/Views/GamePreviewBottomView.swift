import SwiftUI

public struct GamePreviewBottomView: View {
    @ObservedObject var state: AppState
    @State private var position: Int = 8
    @State private var totalRacers: Int = 16
    @State private var currentLap: Int = 1
    @State private var totalLaps: Int = 3
    @State private var currentSpeed: Int = 120

    public init(state: AppState) {
        self.state = state
    }

    public var body: some View {
        ZStack {
            // Simulated game chase cam viewport background
            RoundedRectangle(cornerRadius: 8)
                .fill(
                    LinearGradient(
                        colors: [Color(red: 0.12, green: 0.16, blue: 0.22), Color(red: 0.08, green: 0.08, blue: 0.10)],
                        startPoint: .top,
                        endPoint: .bottom
                    )
                )

            // Game HUD Overlay
            VStack {
                // Top HUD Bar: Position & Lap Counter
                HStack {
                    // Position Banner
                    VStack(alignment: .leading, spacing: 1) {
                        Text("Position")
                            .font(.system(size: 9, weight: .semibold))
                            .foregroundColor(.yellow)
                        Text("\(position)/\(totalRacers)")
                            .font(.system(size: 16, weight: .black, design: .rounded))
                            .foregroundColor(.white)
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 4)
                    .background(Color.black.opacity(0.6))
                    .cornerRadius(6)

                    Spacer()

                    // Lap Counter Banner
                    VStack(alignment: .trailing, spacing: 1) {
                        Text("Lap")
                            .font(.system(size: 9, weight: .semibold))
                            .foregroundColor(.cyan)
                        Text("\(currentLap)/\(totalLaps)")
                            .font(.system(size: 16, weight: .black, design: .rounded))
                            .foregroundColor(.white)
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 4)
                    .background(Color.black.opacity(0.6))
                    .cornerRadius(6)
                }
                .padding(10)

                Spacer()

                // Center: 3D Chase Camera Visual Representation
                HStack {
                    Spacer()
                    VStack(spacing: 4) {
                        Image(systemName: "car.rear.fill")
                            .font(.system(size: 48))
                            .foregroundColor(.red)
                            .shadow(color: .black, radius: 4, x: 0, y: 3)
                        Text(state.vehicle.name)
                            .font(.system(size: 10, weight: .bold))
                            .foregroundColor(.white.opacity(0.8))
                    }
                    Spacer()
                }

                Spacer()

                // Bottom HUD: Speedometer & Tachometer Dial
                HStack {
                    // Damage meter
                    VStack(alignment: .leading, spacing: 2) {
                        Text("VEHICLE INTEGRITY")
                            .font(.system(size: 8, weight: .bold))
                            .foregroundColor(.secondary)
                        ProgressView(value: state.vehicle.healthPercent, total: 100)
                            .tint(.green)
                            .frame(width: 100)
                    }
                    .padding(8)
                    .background(Color.black.opacity(0.6))
                    .cornerRadius(6)

                    Spacer()

                    // Speedometer Dial
                    HStack(spacing: 8) {
                        VStack(alignment: .trailing, spacing: 0) {
                            Text("\(currentSpeed)")
                                .font(.system(size: 22, weight: .black, design: .rounded))
                                .foregroundColor(.white)
                            Text("KM/H")
                                .font(.system(size: 9, weight: .bold))
                                .foregroundColor(.secondary)
                        }

                        // Circular Dial Gauge
                        ZStack {
                            Circle()
                                .stroke(Color.white.opacity(0.15), lineWidth: 4)
                                .frame(width: 44, height: 44)
                            Circle()
                                .trim(from: 0, to: CGFloat(currentSpeed) / 280.0)
                                .stroke(Color.red, style: StrokeStyle(lineWidth: 4, lineCap: .round))
                                .frame(width: 44, height: 44)
                                .rotationEffect(.degrees(-90))
                            Text("4")
                                .font(.system(size: 12, weight: .bold))
                                .foregroundColor(.yellow)
                        }
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 4)
                    .background(Color.black.opacity(0.6))
                    .cornerRadius(6)
                }
                .padding(10)
            }
        }
        .padding(6)
        .background(Color(red: 0.11, green: 0.12, blue: 0.14))
    }
}
