-- ═══════════════════════════════════════════════════════════════════
-- nps_dissector.lua - Wireshark Lua dissector for NPS protocol
--
-- Installation:
--   1. Copy to Wireshark plugins directory:
--      - Linux: ~/.local/lib/wireshark/plugins/
--      - macOS: ~/Library/Application Support/Wireshark/plugins/
--      - Windows: %APPDATA%\Wireshark\plugins\
--   2. Or load via: Wireshark → Analyze → Lua → Evaluate
--   3. Or via command line: wireshark -X lua_script:nps_dissector.lua
--
-- The dissector registers on UDP port 9999 by default.
-- ═══════════════════════════════════════════════════════════════════

-- Create the protocol
local nps_proto = Proto("NPS", "Network Protocol Stack")

-- ── Packet Type Names ────────────────────────────────────────────
local pkt_types = {
    [0x01] = "DATA",
    [0x02] = "ACK",
    [0x03] = "SACK",
    [0x04] = "NACK",
    [0x10] = "SYN",
    [0x11] = "SYN_ACK",
    [0x20] = "FIN",
    [0x21] = "FIN_ACK",
    [0x30] = "RST",
    [0x40] = "PING",
    [0x41] = "PONG",
}

-- ── Flag Names ───────────────────────────────────────────────────
local flag_names = {
    [0] = "SYN",
    [1] = "FIN",
    [2] = "ACK",
    [3] = "SACK",
    [4] = "NACK",
    [5] = "RST",
    [6] = "URG",
}

-- ── Protocol Fields ──────────────────────────────────────────────
local f_version     = ProtoField.uint8("nps.version", "Version", base.DEC)
local f_type        = ProtoField.uint8("nps.type", "Type", base.HEX, pkt_types)
local f_flags       = ProtoField.uint16("nps.flags", "Flags", base.HEX)
local f_seq         = ProtoField.uint32("nps.seq", "Sequence Number", base.DEC)
local f_ack         = ProtoField.uint32("nps.ack", "Acknowledgment Number", base.DEC)
local f_window      = ProtoField.uint16("nps.window", "Window Size", base.DEC)
local f_payload_len = ProtoField.uint16("nps.payload_len", "Payload Length", base.DEC)
local f_ts_send     = ProtoField.uint64("nps.ts_send", "Send Timestamp (us)", base.DEC)
local f_ts_echo     = ProtoField.uint64("nps.ts_echo", "Echo Timestamp (us)", base.DEC)
local f_sack_count  = ProtoField.uint16("nps.sack_count", "SACK Block Count", base.DEC)
local f_conn_id     = ProtoField.uint16("nps.conn_id", "Connection ID", base.DEC)

-- SACK block fields
local f_sack_start  = ProtoField.uint32("nps.sack.start", "SACK Start Seq", base.DEC)
local f_sack_end    = ProtoField.uint32("nps.sack.end", "SACK End Seq", base.DEC)

-- Checksum
local f_checksum    = ProtoField.uint32("nps.checksum", "CRC32 Checksum", base.HEX)

-- Payload
local f_payload     = ProtoField.bytes("nps.payload", "Payload")

-- Flag bits
local f_flag_syn    = ProtoField.bool("nps.flags.syn", "SYN", 16, nil, 0x0001)
local f_flag_fin    = ProtoField.bool("nps.flags.fin", "FIN", 16, nil, 0x0002)
local f_flag_ack    = ProtoField.bool("nps.flags.ack", "ACK", 16, nil, 0x0004)
local f_flag_sack   = ProtoField.bool("nps.flags.sack", "SACK", 16, nil, 0x0008)
local f_flag_nack   = ProtoField.bool("nps.flags.nack", "NACK", 16, nil, 0x0010)
local f_flag_rst    = ProtoField.bool("nps.flags.rst", "RST", 16, nil, 0x0020)
local f_flag_urg    = ProtoField.bool("nps.flags.urg", "URG", 16, nil, 0x0040)

nps_proto.fields = {
    f_version, f_type, f_flags, f_seq, f_ack, f_window, f_payload_len,
    f_ts_send, f_ts_echo, f_sack_count, f_conn_id,
    f_sack_start, f_sack_end, f_checksum, f_payload,
    f_flag_syn, f_flag_fin, f_flag_ack, f_flag_sack,
    f_flag_nack, f_flag_rst, f_flag_urg,
}

-- ── CRC32 Table ──────────────────────────────────────────────────
local crc32_table = {}
local function init_crc32()
    for i = 0, 255 do
        local crc = i
        for _ = 0, 7 do
            if bit32.band(crc, 1) ~= 0 then
                crc = bit32.bxor(bit32.rshift(crc, 1), 0xEDB88320)
            else
                crc = bit32.rshift(crc, 1)
            end
        end
        crc32_table[i] = crc
    end
end
init_crc32()

local function calc_crc32(data, skip_offset, skip_len)
    local crc = 0xFFFFFFFF
    for i = 0, data:len() - 1 do
        -- Skip the CRC field itself
        if i < skip_offset or i >= skip_offset + skip_len then
            local byte = data:get_index(i)
            local idx = bit32.band(bit32.bxor(crc, byte), 0xFF)
            crc = bit32.bxor(bit32.rshift(crc, 8), crc32_table[idx])
        end
    end
    return bit32.bxor(crc, 0xFFFFFFFF)
end

-- ── Dissector Function ───────────────────────────────────────────
function nps_proto.dissector(buffer, pinfo, tree)
    -- Minimum header size: 36 bytes + 4 (CRC)
    if buffer:len() < 40 then return end

    pinfo.cols.protocol:set("NPS")

    local subtree = tree:add(nps_proto, buffer(), "NPS Protocol")
    local offset = 0

    -- Version
    local version = buffer(offset, 1):uint()
    subtree:add(f_version, buffer(offset, 1))
    offset = offset + 1

    -- Type
    local pkt_type = buffer(offset, 1):uint()
    local type_name = pkt_types[pkt_type] or "UNKNOWN"
    subtree:add(f_type, buffer(offset, 1))
    offset = offset + 1

    -- Flags
    local flags = buffer(offset, 2):uint()
    local flags_tree = subtree:add(f_flags, buffer(offset, 2))
    flags_tree:add(f_flag_syn,  buffer(offset, 2))
    flags_tree:add(f_flag_fin,  buffer(offset, 2))
    flags_tree:add(f_flag_ack,  buffer(offset, 2))
    flags_tree:add(f_flag_sack, buffer(offset, 2))
    flags_tree:add(f_flag_nack, buffer(offset, 2))
    flags_tree:add(f_flag_rst,  buffer(offset, 2))
    flags_tree:add(f_flag_urg,  buffer(offset, 2))
    offset = offset + 2

    -- Sequence Number
    local seq = buffer(offset, 4):uint()
    subtree:add(f_seq, buffer(offset, 4))
    offset = offset + 4

    -- ACK Number
    local ack_num = buffer(offset, 4):uint()
    subtree:add(f_ack, buffer(offset, 4))
    offset = offset + 4

    -- Window Size
    subtree:add(f_window, buffer(offset, 2))
    offset = offset + 2

    -- Payload Length
    local payload_len = buffer(offset, 2):uint()
    subtree:add(f_payload_len, buffer(offset, 2))
    offset = offset + 2

    -- Timestamps
    subtree:add(f_ts_send, buffer(offset, 8))
    offset = offset + 8

    subtree:add(f_ts_echo, buffer(offset, 8))
    offset = offset + 8

    -- SACK Count
    local sack_count = buffer(offset, 2):uint()
    subtree:add(f_sack_count, buffer(offset, 2))
    offset = offset + 2

    -- Connection ID
    local conn_id = buffer(offset, 2):uint()
    subtree:add(f_conn_id, buffer(offset, 2))
    offset = offset + 2

    -- SACK Blocks
    if sack_count > 0 and sack_count <= 4 then
        local sack_tree = subtree:add(buffer(offset, sack_count * 8),
                                       "SACK Blocks (" .. sack_count .. ")")
        for i = 1, sack_count do
            local block_tree = sack_tree:add(buffer(offset, 8),
                                              "Block " .. i)
            block_tree:add(f_sack_start, buffer(offset, 4))
            offset = offset + 4
            block_tree:add(f_sack_end, buffer(offset, 4))
            offset = offset + 4
        end
    end

    -- CRC32 Checksum
    local crc_offset = offset
    local crc_val = buffer(offset, 4):uint()
    local crc_item = subtree:add(f_checksum, buffer(offset, 4))
    offset = offset + 4

    -- Verify CRC (compute CRC over header + sack + payload, skipping CRC field)
    local computed_crc = calc_crc32(buffer:bytes(), crc_offset, 4)
    if computed_crc == crc_val then
        crc_item:append_text(" [Valid]")
    else
        crc_item:append_text(string.format(" [INVALID! Expected: 0x%08x]",
                                           computed_crc))
        crc_item:add_expert_info(PI_CHECKSUM, PI_ERROR,
                                  "CRC32 checksum mismatch")
    end

    -- Payload
    if payload_len > 0 and offset + payload_len <= buffer:len() then
        subtree:add(f_payload, buffer(offset, payload_len))
    end

    -- Info column
    local info = string.format("[%s] Seq=%u", type_name, seq)
    if bit32.band(flags, 0x0004) ~= 0 then
        info = info .. string.format(" Ack=%u", ack_num)
    end
    if sack_count > 0 then
        info = info .. string.format(" SACK(%d)", sack_count)
    end
    if payload_len > 0 then
        info = info .. string.format(" Len=%u", payload_len)
    end
    info = info .. string.format(" ConnID=%u", conn_id)

    pinfo.cols.info:set(info)
end

-- ── Register on UDP port 9999 ────────────────────────────────────
local udp_port = DissectorTable.get("udp.port")
udp_port:add(9999, nps_proto)

-- Also register on common alternative ports
udp_port:add(9998, nps_proto)  -- Loss simulator proxy
udp_port:add(10000, nps_proto) -- Alternate port
