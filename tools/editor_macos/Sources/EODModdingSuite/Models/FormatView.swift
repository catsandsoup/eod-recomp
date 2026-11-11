import Foundation

/// Any JSON scalar a format field can hold.
enum FormatValue: Decodable, Equatable {
    case number(Double)
    case text(String)
    case flag(Bool)
    case list([FormatValue])
    case empty

    init(from decoder: Decoder) throws {
        let box = try decoder.singleValueContainer()
        if box.decodeNil() { self = .empty }
        else if let value = try? box.decode(Bool.self) { self = .flag(value) }
        else if let value = try? box.decode(Double.self) { self = .number(value) }
        else if let value = try? box.decode(String.self) { self = .text(value) }
        else if let value = try? box.decode([FormatValue].self) { self = .list(value) }
        else { self = .empty }
    }

    var display: String {
        switch self {
        case .number(let value):
            return value == value.rounded() && abs(value) < 1e15 ? String(Int64(value)) : String(format: "%.6g", value)
        case .text(let value): return value
        case .flag(let value): return value ? "Yes" : "No"
        case .list(let items): return items.map(\.display).joined(separator: ", ")
        case .empty: return ""
        }
    }

    /// JSON object to send back for an edit; nil when the text is not valid for the field type.
    func edited(from text: String, type: String) -> Any? {
        switch type {
        case "f32": return Double(text.trimmingCharacters(in: .whitespaces))
        case "u32", "i32", "u16", "i16", "u8": return Int(text.trimmingCharacters(in: .whitespaces))
        case "bool": return text == "Yes" || text.lowercased() == "true"
        default: return text
        }
    }
}

struct FormatOption: Decodable { let value: FormatValue; let label: String }

struct FormatField: Decodable, Identifiable {
    let id: String
    let group: String?
    let label: String
    let type: String
    let unit: String?
    let value: FormatValue
    let offset: Int?
    let size: Int?
    let editable: Bool?
    let min: Double?
    let max: Double?
    let options: [FormatOption]?
    let confidence: String?
    let explanation: String?
    let evidence: String?
}

struct FormatBlock: Decodable, Identifiable {
    var id: Int { offset }
    let name: String
    let offset: Int
    let size: Int
    let known: Bool
    let explanation: String?
    let text: String?
}

struct FormatPreview: Decodable {
    let kind: String
    let png: String?
    let points: [[Double]]?
    let closed: Bool?
    let columns: [String]?
    let rows: [[FormatValue]]?
}

struct FormatView: Decodable {
    let schema: String
    let fourcc: String
    let title: String
    let summary: String
    let confidence: String
    let editable: Bool
    let fields: [FormatField]
    let blocks: [FormatBlock]
    let preview: FormatPreview?
    let warnings: [String]
}

/// Inspect response for `kind == "format"`.
struct FormatDetail: Decodable {
    let kind: String
    let format: FormatView?
    let hex: String?
    let edited: Bool?
}
