import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import HyprFM
import Quill as Q

Q.Dialog {
    id: root
    anchors.fill: parent
    z: 1000
    dialogWidth: 520
    title: "Connect to Server"
    subtitle: Qt.platform.os === "windows"
        ? "Browse SFTP servers and SMB shares"
        : "Browse SFTP, SMB, and FTP locations through GVFS"
    initialFocusItem: hostField

    property string errorText: ""
    // Windows SFTP login questions: "", "hostkey" or "password".
    property string loginStage: ""
    property string loginHost: ""
    property string loginUser: ""
    property string hostKeyFingerprint: ""
    signal connected(string uri)

    function resetForm() {
        errorText = ""
        loginStage = ""
        passwordField.text = ""
        modeTabs.currentIndex = 0
        hostField.text = ""
        userField.text = ""
        portField.text = ""
        pathField.text = ""
        shareField.text = ""
        directUriField.text = ""
        addBookmarkToggle.checked = true
    }

    function currentUri() {
        if (modeTabs.currentIndex === 3)
            return directUriField.text.trim()

        var protocol = modeTabs.currentIndex === 0 ? "sftp" : modeTabs.currentIndex === 1 ? "smb" : "ftp"
        return remoteAccessService.buildUri(
            protocol,
            hostField.text,
            pathField.text,
            userField.text,
            parseInt(portField.text, 10),
            shareField.text
        )
    }

    // Open prefilled with a location (a bookmark that needs logging in again)
    // and start connecting straight away.
    function connectTo(uri) {
        resetForm()
        modeTabs.currentIndex = 3
        directUriField.text = uri
        addBookmarkToggle.checked = false   // logging in again, not a new place
        open()
        connectCurrentLocation()
    }

    function primaryAction() {
        if (loginStage === "hostkey") {
            loginStage = ""
            remoteAccessService.trustHostKey()
        } else if (loginStage === "password") {
            if (passwordField.text === "") {
                errorText = "Enter the password."
                return
            }
            errorText = ""
            remoteAccessService.providePassword(passwordField.text)
            passwordField.text = ""
        } else {
            connectCurrentLocation()
        }
    }

    function connectCurrentLocation() {
        errorText = ""
        var uri = currentUri()
        if (!uri) {
            errorText = modeTabs.currentIndex === 3
                ? "Enter a remote URI."
                : "Enter a host before connecting."
            return
        }
        remoteAccessService.connectToLocation(uri)
    }

    onOpened: Qt.callLater(function() { hostField.inputItem.forceActiveFocus() })

    Connections {
        target: remoteAccessService

        function onHostKeyConfirmationRequested(host, fingerprint) {
            if (!root.visible)
                return remoteAccessService.cancelLogin()
            root.loginHost = host
            root.hostKeyFingerprint = fingerprint
            root.loginStage = "hostkey"
        }

        function onPasswordRequested(user, host, error) {
            if (!root.visible)
                return remoteAccessService.cancelLogin()
            root.loginUser = user
            root.loginHost = host
            root.errorText = error
            root.loginStage = "password"
            Qt.callLater(function() { passwordField.inputItem.forceActiveFocus() })
        }

        function onConnectionFinished(success, uri, error) {
            if (!root.visible)
                return
            root.loginStage = ""

            if (!success) {
                root.errorText = error || "Couldn't connect to that location."
                return
            }

            if (addBookmarkToggle.checked)
                bookmarks.addBookmark(uri)
            root.connected(uri)
            root.accept()
        }
    }

    Q.Tabs {
        id: modeTabs
        Layout.fillWidth: true
        model: ["SFTP", "SMB", "FTP", "URI"]
    }

    // --- Mode controls ---

    Item {
        id: controlsContainer
        Layout.fillWidth: true
        clip: true
        property real contentHeight: modeTabs.currentIndex === 3
            ? uriControls.implicitHeight
            : modeTabs.currentIndex === 1
                ? smbControls.implicitHeight
                : standardControls.implicitHeight
        implicitHeight: contentHeight

        Behavior on contentHeight {
            NumberAnimation {
                duration: Theme.animDurationSlow
                easing.type: Theme.animEasingEnter; easing.bezierCurve: Theme.animBezierCurve
            }
        }

        // SFTP / FTP controls
        ColumnLayout {
            id: standardControls
            anchors.left: parent.left
            anchors.right: parent.right
            visible: modeTabs.currentIndex === 0 || modeTabs.currentIndex === 2
            spacing: 8

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Text {
                    text: "Host"
                    color: Theme.subtext
                    font.pointSize: Theme.fontSmall
                }

                Q.TextField {
                    id: hostField
                    Layout.fillWidth: true
                    variant: "filled"
                    placeholder: "example.com"
                    onTextChanged: root.errorText = ""
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Text {
                        text: "Username"
                        color: Theme.subtext
                        font.pointSize: Theme.fontSmall
                    }

                    Q.TextField {
                        id: userField
                        Layout.fillWidth: true
                        variant: "filled"
                        placeholder: "Optional"
                        onTextChanged: root.errorText = ""
                    }
                }

                ColumnLayout {
                    Layout.preferredWidth: 140
                    spacing: 4

                    Text {
                        text: "Port"
                        color: Theme.subtext
                        font.pointSize: Theme.fontSmall
                    }

                    Q.TextField {
                        id: portField
                        Layout.fillWidth: true
                        variant: "filled"
                        placeholder: modeTabs.currentIndex === 0 ? "22" : "21"
                        inputItem.validator: IntValidator { bottom: 1; top: 65535 }
                        onTextChanged: root.errorText = ""
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Text {
                    text: "Remote folder"
                    color: Theme.subtext
                    font.pointSize: Theme.fontSmall
                }

                Q.TextField {
                    id: pathField
                    Layout.fillWidth: true
                    variant: "filled"
                    placeholder: "/home/user (optional)"
                    onTextChanged: root.errorText = ""
                }
            }
        }

        // SMB controls
        ColumnLayout {
            id: smbControls
            anchors.left: parent.left
            anchors.right: parent.right
            visible: modeTabs.currentIndex === 1
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 3
                    spacing: 4

                    Text {
                        text: "Host"
                        color: Theme.subtext
                        font.pointSize: Theme.fontSmall
                    }

                    Q.TextField {
                        id: smbHostField
                        Layout.fillWidth: true
                        variant: "filled"
                        placeholder: "Server or NAS host"
                        onTextChanged: {
                            hostField.text = text
                            root.errorText = ""
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    Layout.maximumWidth: 120
                    spacing: 4

                    Text {
                        text: "Port"
                        color: Theme.subtext
                        font.pointSize: Theme.fontSmall
                    }

                    Q.TextField {
                        id: smbPortField
                        Layout.fillWidth: true
                        variant: "filled"
                        placeholder: "445"
                        inputItem.validator: IntValidator { bottom: 1; top: 65535 }
                        onTextChanged: {
                            portField.text = text
                            root.errorText = ""
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Text {
                        text: "Username"
                        color: Theme.subtext
                        font.pointSize: Theme.fontSmall
                    }

                    Q.TextField {
                        id: smbUserField
                        Layout.fillWidth: true
                        variant: "filled"
                        placeholder: "Optional"
                        onTextChanged: {
                            userField.text = text
                            root.errorText = ""
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Text {
                        text: "Share name"
                        color: Theme.subtext
                        font.pointSize: Theme.fontSmall
                    }

                    Q.TextField {
                        id: shareField
                        Layout.fillWidth: true
                        variant: "filled"
                        placeholder: "e.g. public, media"
                        onTextChanged: root.errorText = ""
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Text {
                    text: "Folder inside share"
                    color: Theme.subtext
                    font.pointSize: Theme.fontSmall
                }

                Q.TextField {
                    id: smbPathField
                    Layout.fillWidth: true
                    variant: "filled"
                    placeholder: "Optional"
                    onTextChanged: {
                        pathField.text = text
                        root.errorText = ""
                    }
                }
            }
        }

        // URI controls
        ColumnLayout {
            id: uriControls
            anchors.left: parent.left
            anchors.right: parent.right
            visible: modeTabs.currentIndex === 3
            spacing: 8

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Text {
                    text: "Remote URI"
                    color: Theme.subtext
                    font.pointSize: Theme.fontSmall
                }

                Q.TextField {
                    id: directUriField
                    Layout.fillWidth: true
                    variant: "filled"
                    placeholder: "sftp://user@example.com/home/user"
                    onTextChanged: root.errorText = ""
                }
            }
        }
    }

    // --- Options ---

    Q.Toggle {
        id: addBookmarkToggle
        label: "Add to bookmarks after connecting"
        checked: true
    }

    // --- Separator ---

    Q.Separator { }

    // --- Preview ---

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Text {
            Layout.fillWidth: true
            text: "Preview"
            color: Theme.text
            font.pointSize: Theme.fontNormal
            font.bold: true
        }
    }

    Q.Label {
        Layout.fillWidth: true
        text: currentUri() || "Complete the fields to generate the target URI."
        variant: "body"
        wrapMode: Text.WrapAnywhere
        color: currentUri() ? Theme.text : Theme.subtext
    }

    Q.Label {
        Layout.fillWidth: true
        text: Qt.platform.os === "windows"
            ? "Keys from the ssh-agent and ~/.ssh are tried first; you will be asked for a password if the server needs one."
            : modeTabs.currentIndex === 3
            ? "Use a full GVFS-compatible URI if you already have one."
            : "GVFS will prompt for credentials if the remote server requires them."
        variant: "caption"
        color: Theme.subtext
        wrapMode: Text.WordWrap
    }

    Q.Label {
        Layout.fillWidth: true
        visible: text !== ""
        text: runtimeFeatures.installHint("remoteAccess")
        variant: "caption"
        color: Theme.subtext
        wrapMode: Text.WordWrap
    }

    Q.Label {
        Layout.fillWidth: true
        visible: modeTabs.currentIndex === 1 && text !== ""
        text: runtimeFeatures.installHint("smbRemoteAccess")
        variant: "caption"
        color: Theme.subtext
        wrapMode: Text.WordWrap
    }

    // --- SFTP login questions (Windows) ---

    ColumnLayout {
        Layout.fillWidth: true
        visible: root.loginStage === "hostkey"
        spacing: 4

        Text {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            text: "First connection to " + root.loginHost + ". Check that this key fingerprint matches the server's before trusting it:"
            color: Theme.text
            font.pointSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }

        Q.Label {
            Layout.fillWidth: true
            objectName: "hostKeyFingerprint"
            text: root.hostKeyFingerprint
            variant: "body"
            font.family: Qt.platform.os === "windows" ? "Consolas" : "monospace"
            wrapMode: Text.WrapAnywhere
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        visible: root.loginStage === "password"
        spacing: 4

        Text {
            textFormat: Text.PlainText
            text: "Password for " + root.loginUser + "@" + root.loginHost
            color: Theme.subtext
            font.pointSize: Theme.fontSmall
        }

        Q.TextField {
            id: passwordField
            objectName: "remotePasswordField"
            Layout.fillWidth: true
            variant: "filled"
            placeholder: "Password"
            echoMode: TextInput.Password
            inputItem.Keys.onReturnPressed: root.primaryAction()
        }
    }

    // --- Error message ---

    Text {
        Layout.fillWidth: true
        visible: errorText !== ""
        textFormat: Text.PlainText
        text: errorText
        color: Theme.error
        font.pointSize: Theme.fontSmall
        wrapMode: Text.WordWrap
    }

    // --- Action buttons ---

    RowLayout {
        Layout.alignment: Qt.AlignRight
        spacing: 12

        Q.Button {
            text: "Cancel"
            variant: "ghost"
            size: "small"
            // A Windows SFTP login can be abandoned at any step.
            enabled: Qt.platform.os === "windows" || !remoteAccessService.busy
            onClicked: {
                root.loginStage = ""
                remoteAccessService.cancelLogin()
                root.reject()
            }
        }

        Q.Button {
            text: remoteAccessService.busy ? "Connecting..."
                : root.loginStage === "hostkey" ? "Trust and Connect"
                : root.loginStage === "password" ? "Log In"
                : "Connect"
            variant: "primary"
            size: "small"
            enabled: !remoteAccessService.busy
            onClicked: root.primaryAction()
        }
    }
}
