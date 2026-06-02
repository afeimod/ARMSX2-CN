// RetroAchievementsSettingsView.swift — RetroAchievements account and status UI
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private let retroAchievementsNotification = Notification.Name("ARMSX2RetroAchievementsStateChanged")

struct RetroAchievementsSettingsView: View {
    @State private var settings = SettingsStore.shared
    @State private var state: [String: Any] = [:]
    @State private var achievementsEnabled = false
    @State private var hardcoreEnabled = false
    @State private var notificationsEnabled = true
    @State private var leaderboardNotificationsEnabled = true
    @State private var overlaysEnabled = true
    @State private var username = ""
    @State private var password = ""
    @State private var showingLogin = false
    @State private var loggingIn = false
    @State private var messageTitle = ""
    @State private var messageBody = ""
    @State private var showingMessage = false

    var body: some View {
        Form {
            Section {
                Toggle(settings.localized("Enable RetroAchievements"), isOn: Binding(
                    get: { achievementsEnabled },
                    set: { newValue in
                        achievementsEnabled = newValue
                        ARMSX2Bridge.setRetroAchievementsEnabled(newValue)
                        refreshSoon()
                    }
                ))

                statusRow("Client", value: bool("active") ? "Active" : "Inactive")
                statusRow("Account", value: accountSummary, localizeValue: !bool("loggedIn"))
            } header: {
                Text(settings.localized("RetroAchievements"))
            } footer: {
                Text(settings.localized("Uses the same achievements core as ARMSX2 Android. Login tokens are stored in ARMSX2's local config; passwords are not stored by this screen."))
            }

            Section(settings.localized("Account")) {
                if bool("loggedIn") {
                    statusRow("User", value: displayName, localizeValue: false)
                    statusRow("Points", value: "\(int("points")) hard / \(int("softcorePoints")) soft", localizeValue: false)
                    if int("unreadMessages") > 0 {
                        statusRow("Messages", value: "\(int("unreadMessages")) \(settings.localized("unread"))", localizeValue: false)
                    }

                    Button(role: .destructive) {
                        ARMSX2Bridge.logoutRetroAchievements()
                        refreshSoon()
                    } label: {
                        Text(settings.localized("Log Out"))
                    }
                } else {
                    Button {
                        username = string("username")
                        password = ""
                        showingLogin = true
                    } label: {
                        Text(settings.localized(loggingIn ? "Logging In..." : "Log In"))
                    }
                    .disabled(!achievementsEnabled || loggingIn)
                }
            }

            Section {
                Toggle(settings.localized("Hardcore Mode"), isOn: Binding(
                    get: { hardcoreEnabled },
                    set: { newValue in
                        hardcoreEnabled = newValue
                        ARMSX2Bridge.setRetroAchievementsHardcore(newValue)
                        refreshSoon()
                    }
                ))
                .disabled(!achievementsEnabled)

                Toggle(settings.localized("Achievement Notifications"), isOn: Binding(
                    get: { notificationsEnabled },
                    set: { newValue in
                        notificationsEnabled = newValue
                        ARMSX2Bridge.setRetroAchievementsNotifications(newValue)
                        refreshSoon()
                    }
                ))
                .disabled(!achievementsEnabled)

                Toggle(settings.localized("Leaderboard Notifications"), isOn: Binding(
                    get: { leaderboardNotificationsEnabled },
                    set: { newValue in
                        leaderboardNotificationsEnabled = newValue
                        ARMSX2Bridge.setRetroAchievementsLeaderboards(newValue)
                        refreshSoon()
                    }
                ))
                .disabled(!achievementsEnabled)

                Toggle(settings.localized("In-Game Overlays"), isOn: Binding(
                    get: { overlaysEnabled },
                    set: { newValue in
                        overlaysEnabled = newValue
                        ARMSX2Bridge.setRetroAchievementsOverlays(newValue)
                        refreshSoon()
                    }
                ))
                .disabled(!achievementsEnabled)
            } header: {
                Text(settings.localized("Modes"))
            } footer: {
                Text(settings.localized("Hardcore is enforced by the core and can restrict cheats, save states, and other non-hardcore features while active."))
            }

            Section(settings.localized("Current Game")) {
                if bool("hasActiveGame") {
                    statusRow("Title", value: string("gameTitle", fallback: settings.localized("Unknown Game")), localizeValue: false)
                    statusRow("Game ID", value: "\(int("gameId"))", localizeValue: false)

                    if int("totalAchievements") > 0 {
                        statusRow("Achievements", value: "\(int("unlockedAchievements")) / \(int("totalAchievements"))", localizeValue: false)
                        statusRow("Points", value: "\(int("unlockedPoints")) / \(int("totalPoints"))", localizeValue: false)
                    } else if bool("hasAchievements") {
                        statusRow("Achievements", value: "Loaded")
                    } else {
                        statusRow("Achievements", value: "None found")
                    }

                    if bool("hasLeaderboards") {
                        statusRow("Leaderboards", value: "Available")
                    }

                    if bool("hasRichPresence") {
                        statusRow("Rich Presence", value: string("richPresence", fallback: settings.localized("Active")), localizeValue: false)
                    }
                } else {
                    Text(settings.localized("Boot a game while RetroAchievements is enabled to see game progress here."))
                        .foregroundStyle(.secondary)
                }
            }
        }
        .navigationTitle(settings.localized("RetroAchievements"))
        .navigationBarTitleDisplayMode(.inline)
        .onAppear(perform: refresh)
        .onReceive(NotificationCenter.default.publisher(for: retroAchievementsNotification)) { _ in
            refresh()
        }
        .alert(settings.localized("RetroAchievements Login"), isPresented: $showingLogin) {
            TextField(settings.localized("Username"), text: $username)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            SecureField(settings.localized("Password"), text: $password)
            Button(settings.localized("Log In")) {
                beginLogin()
            }
            Button(settings.localized("Cancel"), role: .cancel) {}
        } message: {
            Text(settings.localized("Use your RetroAchievements account credentials."))
        }
        .alert(settings.localized(messageTitle), isPresented: $showingMessage) {
            Button(settings.localized("OK"), role: .cancel) {}
        } message: {
            Text(messageBody)
        }
    }

    private var accountSummary: String {
        if bool("loggedIn") {
            return displayName
        }
        return achievementsEnabled ? "Not logged in" : "Disabled"
    }

    private var displayName: String {
        string("displayName", fallback: string("username", fallback: "Logged in"))
    }

    private func statusRow(_ title: String, value: String, localizeValue: Bool = true) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(settings.localized(title))
            Spacer(minLength: 16)
            Text(localizeValue ? settings.localized(value) : value)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.trailing)
        }
    }

    private func refresh() {
        state = ARMSX2Bridge.retroAchievementsState()
        achievementsEnabled = bool("enabled")
        hardcoreEnabled = bool("hardcorePreference")
        notificationsEnabled = bool("notifications", fallback: true)
        leaderboardNotificationsEnabled = bool("leaderboardNotifications", fallback: true)
        overlaysEnabled = bool("overlays", fallback: true)
    }

    private func refreshSoon() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) {
            refresh()
        }
    }

    private func beginLogin() {
        let trimmedUsername = username.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmedUsername.isEmpty, !password.isEmpty else {
            showMessage(title: "Missing Login", body: settings.localized("Enter both your RetroAchievements username and password."))
            return
        }

        loggingIn = true
        ARMSX2Bridge.loginRetroAchievements(username: trimmedUsername, password: password) { success, message in
            Task { @MainActor in
                loggingIn = false
                password = ""
                refresh()
                showMessage(title: success ? "Logged In" : "Login Failed", body: settings.localized(message))
            }
        }
    }

    private func showMessage(title: String, body: String) {
        messageTitle = title
        messageBody = body
        showingMessage = true
    }

    private func bool(_ key: String, fallback: Bool = false) -> Bool {
        state[key] as? Bool ?? fallback
    }

    private func int(_ key: String, fallback: Int = 0) -> Int {
        if let value = state[key] as? Int {
            return value
        }
        if let value = state[key] as? NSNumber {
            return value.intValue
        }
        return fallback
    }

    private func string(_ key: String, fallback: String = "") -> String {
        guard let value = state[key] as? String, !value.isEmpty else {
            return fallback
        }
        return value
    }
}
