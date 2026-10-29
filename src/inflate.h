// gzip (RFC 1952) around one deflate stream (RFC 1951), for VGZ game music files, and raw deflate for packs. Single-member
// gzip only, as VGZ uses; the whole input is held in memory and the caller caps the output size.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// True if the bytes start with the gzip magic number.
bool IsGzip(const unsigned char* in, size_t n);

// Inflates a single-member gzip file into `out`. The CRC-32 and length in the trailer must
// match. False on any damage, or when the output would be larger than maxOut bytes.
bool Gunzip(const unsigned char* in, size_t n, size_t maxOut, std::string& out);

// Inflates one raw deflate stream (RFC 1951), as a ZIP entry stores it. False on damage, or when
// the output would be larger than maxOut bytes.
bool Inflate(const unsigned char* in, size_t n, size_t maxOut, std::string& out);

// CRC-32 as used by gzip and PNG (reflected, polynomial 0xEDB88320).
uint32_t Crc32(const unsigned char* p, size_t n, uint32_t crc = 0);
