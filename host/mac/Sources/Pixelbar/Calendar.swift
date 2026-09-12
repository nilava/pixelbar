// What is on your calendar, without asking Google for permission.
//
// EventKit reads whatever accounts Calendar.app already has — Google, Exchange,
// iCloud, a subscribed ICS — so there is no OAuth client, no consent screen, no
// client secret in a repo, and no refresh token to expire quietly at the worst
// moment. One permission prompt on first run, and it works offline against the
// local store.
//
// The cost is that it sees what Calendar.app sees. An account not added there
// is invisible here, which is a fair trade for not implementing an identity
// provider inside a desk ornament.
import EventKit
import Foundation

struct Meeting {
    let title: String
    let start: Date
    let end: Date
    /// A video call rather than a room booking — worth knowing, because a
    /// video call plus a live camera is a different thing from a blocked hour.
    let isVideo: Bool

    var isNow: Bool {
        let now = Date()
        return now >= start && now < end
    }
    var startsIn: TimeInterval { start.timeIntervalSinceNow }
}

actor Calendars {
    private let store = EKEventStore()
    private(set) var authorised = false

    /// Asks once. A refusal is remembered by the system, so this is not
    /// something to retry in a loop — the menu offers it again instead.
    func requestAccess() async -> Bool {
        do {
            if #available(macOS 14.0, *) {
                authorised = try await store.requestFullAccessToEvents()
            } else {
                authorised = try await store.requestAccess(to: .event)
            }
        } catch {
            authorised = false
        }
        return authorised
    }

    /// Everything starting or running in the next `window`, soonest first.
    func upcoming(window: TimeInterval = 3600) -> [Meeting] {
        guard authorised else { return [] }
        let now = Date()
        let pred = store.predicateForEvents(withStart: now.addingTimeInterval(-3600),
                                            end: now.addingTimeInterval(window),
                                            calendars: nil)
        return store.events(matching: pred)
            .filter { !$0.isAllDay }
            // Declined invitations are not meetings you are in. Without this
            // the panel announces every event you said no to.
            .filter { !isDeclined($0) }
            .filter { ($0.endDate ?? now) > now }
            .map {
                Meeting(title: $0.title ?? "Meeting",
                        start: $0.startDate,
                        end: $0.endDate,
                        isVideo: Self.looksLikeVideo($0))
            }
            .sorted { $0.start < $1.start }
    }

    private func isDeclined(_ e: EKEvent) -> Bool {
        guard let me = e.attendees?.first(where: { $0.isCurrentUser }) else { return false }
        return me.participantStatus == .declined
    }

    /// Whether this event is somewhere you join rather than somewhere you go.
    ///
    /// A URL, a location or a body carrying a known joining link. Deliberately
    /// a short list of hosts rather than a general URL match: "there is a link
    /// in the notes" is true of most meetings and means nothing.
    private static func looksLikeVideo(_ e: EKEvent) -> Bool {
        let hosts = ["meet.google.com", "zoom.us", "teams.microsoft.com",
                     "webex.com", "whereby.com", "meet.jit.si"]
        let haystack = [e.url?.absoluteString, e.location, e.notes]
            .compactMap { $0 }.joined(separator: " ").lowercased()
        return hosts.contains { haystack.contains($0) }
    }
}
