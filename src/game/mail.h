#pragma once

// In-game mail over the Universal Chat Service (MailMgr, CMailWnd,
// CMailCompositionWnd in the original). The client keeps the mailboxes it may
// read (the account's characters), the headers of the selected mailbox and
// the bodies fetched so far; commands go to the chat server as text
// ("[getheaders", "[getbody 12", "[setmessagestatus READ 12", "[selectmailbox
// 1", "[mailto ..."), see net/chat_server.h. The rules here follow the original's MailMgr_*
// functions (addresses at each function).

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::game {

// MailMgr_SetMessageStatus @0x46af20's words, in its numbering.
enum class MailStatus : std::uint8_t { New = 0, Unread = 1, Read = 2, Trash = 3, Deleted = 4 };
const char* mail_status_word(MailStatus s); // "NEW", "UNREAD", "READ", "TRASH", "DELETED"
// The UCS's header status (EQEmu's mail table: 1 new, 3 read, 4 trash) as the
// client stores it (0x533650: 2 → unread, 3 → read, 4 → trash, 5 → deleted, else new).
MailStatus mail_status_from_server(std::int32_t server_status);

// The mail address book (CMailAddressBookWnd, AddressBook_* in the original). An entry is a name for one or more
// addresses; a private entry's addresses go out hidden (the UCS's '-' prefix). The file is the original's
// (MailMgr_LoadUserFiles @0x46dd20, AddressBook_Save @0x469dc0, AddressBook_Parse @0x46c8b0):
//   EverQuest Address Book v1.0
//   # WARNING: ...                       three comment lines
//   Name: <name>
//   Addresses: <address> <address> ...   qualified (SOE.EQ.<server>.<name>)
//   Flags: PRIVATE                       "Flags: " alone for a public entry
//   Note: <first line of the note>
//   <more lines of the note>
//   <an empty line ends the entry>
struct MailAddressEntry {
    std::string name;
    std::vector<std::string> addresses; // qualified when the entry is stored (AddressBookEntry_Ctor)
    bool is_private = false;
    std::string note;                   // AddressBookEntry_NormalizeNote's form
};

struct MailAddressBook {
    std::vector<MailAddressEntry> entries; // sorted by name, case-insensitive

    // AddressBook_FindEntryByName: case-insensitive, the name trimmed.
    const MailAddressEntry* find(std::string_view name) const;
    // AddressBook_AddOrUpdateEntry @0x46c6b0: adds the entry, in name order, or replaces the one with its name (the
    // Add / Modify button); an empty name is refused. The note is normalized and, given the server, the addresses
    // qualified, as AddressBookEntry_Ctor @0x46c1a0 does.
    bool put(MailAddressEntry e, const std::string& server = {});
    bool remove(std::string_view name);

    // AddressBook_Save's text: the header, the comments and every entry (AddressBook_WriteEntry @0x468fe0).
    std::string save_text() const;
    // AddressBook_Parse: false when the header or an entry is malformed (the entries before it are kept). A text
    // in this client's earlier format (one tab-separated entry a line) is read as well. The server qualifies the
    // addresses (the file's are qualified already when the game wrote it).
    static bool parse(std::string_view text, const std::string& server, MailAddressBook& out);
    static MailAddressBook parse(std::string_view text, const std::string& server = {}) {
        MailAddressBook b;
        parse(text, server, b);
        return b;
    }
};

// AddressBookEntry_NormalizeNote @0x469fa0: trailing blanks and line breaks go, and an empty line inside the note
// gets a space (the file's empty line ends an entry).
std::string mail_normalize_note(std::string_view note);

// Splits an address field ("a, b c") into addresses: commas and blanks separate them.
std::vector<std::string> mail_split_addresses(std::string_view field);

struct MailHeader {
    std::uint32_t id = 0;        // the message id (msgid)
    std::uint32_t timestamp = 0; // time_t
    MailStatus status = MailStatus::New;
    std::string from;            // "SOE.EQ.<server>.<name>"
    std::string subject;
    std::optional<std::string> body; // fetched with getbody
    std::string to;                  // the body packet's recipients
};

struct MailState {
    std::vector<std::string> mailboxes; // MailMgr +0xc: from OP_MailLogin's list
    int current = 0;                    // MailMgr +0x1c
    std::map<std::uint32_t, MailHeader> headers; // the current mailbox's, by id
    std::int32_t expected = -1;  // OP_MailHeaderCount's count (-1 before one came)
    bool unread = false;         // MailMgr +0x5c: a new or unread header is there
    std::uint32_t max_id = 0;    // MailMgr +0x68
    bool window_open = false;
    std::vector<std::string> delivery; // the last delivery results (eqstr 4530 / 4531 lines)
    MailAddressBook address_book;      // loaded and saved by the app (app/lua_ui_mail.cpp)
    MailAddressBook ignore_list;       // MailMgr +0xa4, the same kind of book (IgnoreList.txt), loaded and saved alike

    // MailMgr_StoreHeader / OnHeader: stores a header (replacing one with the same id).
    void store(MailHeader h);
    std::vector<const MailHeader*> inbox() const;   // not trash / deleted, newest first
    std::vector<const MailHeader*> trash() const;   // trash
};

// Mail_QualifyAddress @0x469140: "name" → "SOE.EQ.<server>.name", "server.name"
// → "SOE.EQ.server.name", "EQ.server.name" → "SOE.EQ.server.name"; spaces
// become underscores.
std::string mail_qualify_address(std::string address, const std::string& server);
// Mail_ExpandRecipients @0x46a120: the comma list, trimmed, qualified,
// de-duplicated and joined with ", "; a leading '-' (a hidden recipient) is
// kept; an entry with '<' is passed unchanged. A name the address book (or
// `groups`, the built-in "Guild" and "Group" lists) holds becomes
// "Name <addr addr ...>", hidden when typed with '-' or the entry is private,
// as EQEmu's UCS reads it (ucs/clientlist.cpp ParseRecipients).
std::string mail_expand_recipients(const std::string& list, const std::string& server,
                                   const MailAddressBook* book = nullptr,
                                   const std::vector<MailAddressEntry>* groups = nullptr);
// MailMgr_SendMail @0x46be70's command: "[mailto \"<recipients>\" <subject, quoted
// when it has a space> <body>".
std::string mail_to_command(const std::string& recipients, const std::string& subject, const std::string& body,
                            const std::string& server, const MailAddressBook* book = nullptr,
                            const std::vector<MailAddressEntry>* groups = nullptr);
// The name in an address: the part after the last '.' ("SOE.EQ.x.Name" → "Name").
std::string mail_short_name(const std::string& address);

} // namespace hailstone::game
