import Foundation

/// Any JSON scalar a format field can hold.
enum FormatValue: Decodable, Equatable, Sendable {
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
    /// File-layout detail; shown only with "Show file internals".
    let expert: Bool?
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
    let width: Int?
    let height: Int?
    let points: [[Double]]?
    /// Path previews: the node number of each point, so the table and the drawing agree.
    let nodes: [Int]?
    let closed: Bool?
    let unit: String?
    /// Path previews: caption for the first point ("Start/finish").
    let start: String?
    /// Path previews: draw direction arrows along the line.
    let direction: Bool?
    let columns: [FormatColumn]?
    let rows: [[FormatValue]]?
}

/// A column of a `table`. Older views list plain strings; newer ones describe each column.
struct FormatColumn: Decodable, Identifiable, Hashable {
    let key: String
    let label: String
    let unit: String?
    let type: String?
    /// Edit id template, e.g. "node.{id}.x" (`{id}` is the row's first cell). Absent = read-only.
    let edit: String?
    let min: Double?
    let max: Double?
    /// Text columns: how a line break is shown and typed (e.g. "\\n").
    let newline: String?
    var id: String { key }

    init(from decoder: Decoder) throws {
        if let single = try? decoder.singleValueContainer(), let text = try? single.decode(String.self) {
            key = text; label = text; unit = nil; type = nil; edit = nil; min = nil; max = nil; newline = nil
            return
        }
        let box = try decoder.container(keyedBy: Keys.self)
        key = try box.decode(String.self, forKey: .key)
        label = try box.decodeIfPresent(String.self, forKey: .label) ?? key
        unit = try box.decodeIfPresent(String.self, forKey: .unit)
        type = try box.decodeIfPresent(String.self, forKey: .type)
        edit = try box.decodeIfPresent(String.self, forKey: .edit)
        min = try box.decodeIfPresent(Double.self, forKey: .min)
        max = try box.decodeIfPresent(Double.self, forKey: .max)
        newline = try box.decodeIfPresent(String.self, forKey: .newline)
    }
    private enum Keys: String, CodingKey { case key, label, unit, type, edit, min, max, newline }
}

/// A large set of rows shown as a scrolling table instead of one field per value.
struct FormatTable: Decodable {
    let name: String?
    /// "path" (racing line nodes), "strings" (game text), or absent.
    let role: String?
    let searchable: Bool?
    let columns: [FormatColumn]
    let rows: [[FormatValue]]
}

struct FormatHow: Decodable, Identifiable {
    let text: String
    let confidence: String?
    var id: String { text }
}

/// A suggested next step, e.g. "Change handling in this car's physics settings".
struct FormatAction: Decodable {
    let label: String
    let archive: String?
    let asset: String?
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
    let table: FormatTable?
    let howWeKnow: [FormatHow]?
    let notes: [String]?
    /// "unused" (the game never reads this file) or "empty".
    let status: String?
    let action: FormatAction?
}
