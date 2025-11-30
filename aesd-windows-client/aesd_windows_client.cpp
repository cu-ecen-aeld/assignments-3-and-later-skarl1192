#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <iostream>
#include <string>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <conio.h> // For _getch()

#define RPI_LCD_SERVER_PORT 9000
#define RPI_LCD_SERVER_IP "192.168.1.2"

/* --- Global State for Toggles --- */
bool g_backlightOn = true;
bool g_displayOn = true;
bool g_cursorOn = false;
bool g_blinkOn = false;
bool g_textDirLtr = true;
bool g_autoscrollOn = false;

/* --- Helper Functions --- */

/* Appends a newline and sends the string over TCP */
void sendCommand(SOCKET s, const std::string& cmd) {
    std::string packet = cmd + "\n";
    int iResult = send(s, packet.c_str(), (int)packet.length(), 0);
    if (iResult == SOCKET_ERROR) {
        std::cerr << "\n[Error] Send failed: " << WSAGetLastError() << std::endl;
        exit(1);
    }
}

/* Prints the action description to the console and re-prints the prompt */
void logAction(const std::string& msg) {
    std::cout << "\n[CMD] " << msg << "\n> ";
}

/* Displays the key mapping menu */
void printInstructions() {
    std::cout << "Instructions:\n";
    std::cout << "  Type text + ENTER : Send text to LCD\n";
    std::cout << "  LEFT/RIGHT Arrow  : Scroll Display\n";
    std::cout << "  F1                : Clear Screen\n";
    std::cout << "  F2                : Return Home\n";
    std::cout << "  F3                : Toggle Backlight\n";
    std::cout << "  F4                : Toggle Display On/Off\n";
    std::cout << "  F5                : Toggle Underline Cursor\n";
    std::cout << "  F6                : Toggle Cursor Blink\n";
    std::cout << "  F7                : Toggle Text Direction\n";
    std::cout << "  F8                : Toggle Autoscroll\n";
    std::cout << "  ESC               : Exit\n";
    std::cout << "---------------------------------------------\n";
    std::cout << "> ";
}

int main() {
    /* 1. Initialize Winsock */
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != 0) {
        std::cerr << "WSAStartup failed: " << iResult << std::endl;
        return 1;
    }

    std::cout << "=== AESD LCD Remote Client (C++) ===\n";
    
    /* 2. Connect to Server */
    SOCKET connectSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connectSocket == INVALID_SOCKET) {
        std::cerr << "Error at socket(): " << WSAGetLastError() << std::endl;
        WSACleanup();
        return 1;
    }

    sockaddr_in clientService;
    clientService.sin_family = AF_INET;
    clientService.sin_addr.s_addr = inet_addr(RPI_LCD_SERVER_IP);
    clientService.sin_port = htons(RPI_LCD_SERVER_PORT);

    std::cout << "Connecting to " << RPI_LCD_SERVER_IP << ":" << RPI_LCD_SERVER_PORT << "...\n";
    if (connect(connectSocket, (SOCKADDR*)&clientService, sizeof(clientService)) == SOCKET_ERROR) {
        std::cerr << "Failed to connect.\n";
        closesocket(connectSocket);
        WSACleanup();
        return 1;
    }

    std::cout << "Connected!\n";
    printInstructions();

    /* 3. Input Loop */
    std::string textBuffer = "";
    bool running = true;

    while (running) {
        /* _getch() gets a character without waiting for Enter */
        int ch = _getch();

        /* 0 or 0xE0 indicates a special key (function key or arrow key) */
        if (ch == 0 || ch == 0xE0) {
            int scancode = _getch(); // Read the second byte

            switch (scancode) {
            /* --- Arrow Keys (Prefix 0xE0) --- */
            case 75: // Left Arrow
                sendCommand(connectSocket, "LCD:SCROLL:0");
                logAction("Scroll Left");
                /* Clear buffer visuals if user was typing */
                textBuffer = ""; 
                break;
            case 77: // Right Arrow
                sendCommand(connectSocket, "LCD:SCROLL:1");
                logAction("Scroll Right");
                textBuffer = "";
                break;

            /* --- Function Keys (Prefix usually 0) --- */
            case 59: // F1
                sendCommand(connectSocket, "LCD:CLEAR");
                logAction("Clear Screen");
                break;
            case 60: // F2
                sendCommand(connectSocket, "LCD:HOME");
                logAction("Return Home");
                break;
            case 61: // F3
                g_backlightOn = !g_backlightOn;
                sendCommand(connectSocket, "LCD:BACKLIGHT:" + std::to_string(g_backlightOn ? 1 : 0));
                logAction(std::string("Backlight ") + (g_backlightOn ? "ON" : "OFF"));
                break;
            case 62: // F4
                g_displayOn = !g_displayOn;
                sendCommand(connectSocket, "LCD:DISPLAY:" + std::to_string(g_displayOn ? 1 : 0));
                logAction(std::string("Display ") + (g_displayOn ? "ON" : "OFF"));
                break;
            case 63: // F5
                g_cursorOn = !g_cursorOn;
                sendCommand(connectSocket, "LCD:UNDERLINE:" + std::to_string(g_cursorOn ? 1 : 0));
                logAction(std::string("Cursor ") + (g_cursorOn ? "ON" : "OFF"));
                break;
            case 64: // F6
                g_blinkOn = !g_blinkOn;
                sendCommand(connectSocket, "LCD:BLINK:" + std::to_string(g_blinkOn ? 1 : 0));
                logAction(std::string("Blink ") + (g_blinkOn ? "ON" : "OFF"));
                break;
            case 65: // F7
                g_textDirLtr = !g_textDirLtr;
                sendCommand(connectSocket, "LCD:TEXTDIR:" + std::to_string(g_textDirLtr ? 1 : 0));
                logAction(std::string("Direction ") + (g_textDirLtr ? "LTR" : "RTL"));
                break;
            case 66: // F8
                g_autoscrollOn = !g_autoscrollOn;
                sendCommand(connectSocket, "LCD:AUTOSCROLL:" + std::to_string(g_autoscrollOn ? 1 : 0));
                logAction(std::string("Autoscroll ") + (g_autoscrollOn ? "ON" : "OFF"));
                break;
            }
        }
        else {
            /* --- Standard Keys --- */
            if (ch == 27) { // ESC
                running = false;
            }
            else if (ch == 13) { // ENTER
                if (!textBuffer.empty()) {
                    sendCommand(connectSocket, textBuffer);
                    textBuffer = "";
                    std::cout << "\n> "; // New prompt
                }
            }
            else if (ch == 8) { // BACKSPACE
                if (!textBuffer.empty()) {
                    textBuffer.pop_back();
                    std::cout << "\b \b"; // Erase char visually
                }
            }
            else {
                /* Printable characters */
                textBuffer += (char)ch;
                std::cout << (char)ch;
            }
        }
    }

    /* 4. Cleanup */
    closesocket(connectSocket);
    WSACleanup();

    std::cout << "\nDisconnected.\n";
    return 0;
}