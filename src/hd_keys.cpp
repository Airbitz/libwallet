/*
 * Copyright (c) 2011-2013 libwallet developers (see AUTHORS)
 *
 * This file is part of libwallet.
 *
 * libwallet is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <wallet/define.hpp>
#include <wallet/hd_keys.hpp>

#ifdef USE_OPENSSL_EC
#include <openssl/ec.h>
#endif
#ifdef USE_OPENSSL_HM
#include <openssl/hmac.h>
#endif
#ifdef USE_OPENSSL_BN
#include <openssl/bn.h>
#endif

#include <algorithm>
#include <bitcoin/bitcoin.hpp>

namespace libwallet {

template<typename T, void destroy(T* p)>
class auto_free
{
public:
    auto_free(T* p)
      : ptr(p)
    {
    }
    ~auto_free()
    {
        destroy(ptr);
    }
    operator T*()
    {
        return ptr;
    }
    T* ptr;
};

typedef auto_free<BIGNUM, BN_free> ssl_bignum;
typedef auto_free<BN_CTX, BN_CTX_free> ssl_bn_ctx;

// ****************************************************************************
typedef auto_free<EC_GROUP, EC_GROUP_free> ssl_ec_group;
typedef auto_free<EC_POINT, EC_POINT_free> ssl_ec_point;

secret_parameter secp256k1_n
{
    {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
        0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b,
        0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41
    }
};
// ****************************************************************************

constexpr uint32_t mainnet_private_prefix = 0x0488ADE4;
constexpr uint32_t mainnet_public_prefix = 0x0488B21E;
constexpr uint32_t testnet_private_prefix = 0x04358394;
constexpr uint32_t testnet_public_prefix = 0x043587CF;

static data_chunk secret_to_public_key(const secret_parameter& secret)
{
    elliptic_curve_key key;
    key.set_secret(secret);
    return key.public_key();
}

// long_hash is used for hmac_sha512 in libbitcoin
constexpr size_t half_long_hash_size = long_hash_size / 2;
typedef std::array<uint8_t, half_long_hash_size> half_long_hash;

/**
 * Corresponds to a split HMAC-SHA256 result, as used in BIP 32.
 */
struct split_long_hash
{
    half_long_hash L;
    half_long_hash R;
};

static split_long_hash split(const long_hash& hmac)
{
    split_long_hash I;
    std::copy(hmac.begin(), hmac.begin() + half_long_hash_size, I.L.begin());
    std::copy(hmac.begin() + half_long_hash_size, hmac.end(), I.R.begin());
    return I;
}

static ser32_type ser32(uint32_t i)
{
    ser32_type out;
    out[0] = 0xff & (i >> 24);
    out[1] = 0xff & (i >> 16);
    out[2] = 0xff & (i >> 8);
    out[3] = 0xff & i;
    return out;
}

BCW_API hd_public_key::hd_public_key()
  : valid_(false)
{
}

BCW_API hd_public_key::hd_public_key(const data_chunk& public_key,
    const chain_code_type& chain_code, hd_key_lineage lineage)
  : valid_(true), K_(public_key), c_(chain_code), lineage_(lineage)
{
}

BCW_API bool hd_public_key::valid() const
{
    return valid_;
}

BCW_API const data_chunk& hd_public_key::public_key() const
{
    return K_;
}

BCW_API const chain_code_type& hd_public_key::chain_code() const
{
    return c_;
}

BCW_API const hd_key_lineage& hd_public_key::lineage() const
{
    return lineage_;
}

BCW_API std::string hd_public_key::serialize() const
{
    data_chunk data;
    data.reserve(4 + 1 + 4 + 4 + 32 + 33 + 4);

    extend_data(data, ser32(lineage_.testnet ? testnet_public_prefix :
        mainnet_public_prefix));
    data.push_back(lineage_.depth);
    extend_data(data, lineage_.parent_fingerprint);
    extend_data(data, ser32(lineage_.child_number));
    extend_data(data, c_);
    extend_data(data, K_);

    extend_data(data, uncast_type(bitcoin_checksum(data)));
    return encode_base58(data);
}

BCW_API ser32_type hd_public_key::fingerprint() const
{
    short_hash md = bitcoin_short_hash(K_);
    return ser32_type{{md[0], md[1], md[2], md[3]}};
}

BCW_API payment_address hd_public_key::address() const
{
    payment_address address;
    set_public_key(address, K_);
    return address;
}

BCW_API hd_public_key hd_public_key::generate_public_key(uint32_t i)
{
    if (!valid_)
        return hd_private_key();
    if (first_hardened_key <= i)
        return hd_public_key();

    data_chunk data;
    data.reserve(33 + 4);
    extend_data(data, K_);
    extend_data(data, ser32(i));
    auto I = split(hmac_sha512_hash(data, to_data_chunk(c_)));

    // ************************************************************************
    // The returned child key Ki is point(parse256(IL)) + Kpar.
    ssl_ec_group group = EC_GROUP_new_by_curve_name(NID_secp256k1);

    if (!group)
        return hd_public_key();

    ssl_bn_ctx ctx = BN_CTX_new();
    ssl_ec_point Ki = EC_POINT_new(group);
    ssl_ec_point Kpar = EC_POINT_new(group);
    ssl_ec_point IL = EC_POINT_new(group);

    ssl_bignum il = BN_bin2bn(I.L.data(), (int)I.L.size(), nullptr);
    ssl_bignum n = BN_bin2bn(secp256k1_n.data(), (int)secp256k1_n.size(),
        nullptr);

    if (!ctx || !Ki || !Kpar || !IL || !il || !n)
        return hd_public_key();

    if (!EC_POINT_oct2point(group, Kpar, K_.data(), K_.size(), ctx))
        return hd_public_key();
    if (!EC_POINT_mul(group, IL, il, nullptr, nullptr, ctx))
        return hd_public_key();
    if (!EC_POINT_add(group, Ki, IL, Kpar, ctx))
        return hd_public_key();

    // The key is invalid if parse256(IL) >= n or Ki is at infinity:
    if (0 <= BN_cmp(il, n) || EC_POINT_is_at_infinity(group, Ki))
        return hd_private_key();

    size_t Ki_size = EC_POINT_point2oct(group, Ki, POINT_CONVERSION_COMPRESSED,
        NULL, 0, ctx);

    data_chunk out(Ki_size);
    if (!EC_POINT_point2oct(group, Ki, POINT_CONVERSION_COMPRESSED, out.data(),
        out.size(), ctx))
        return hd_public_key();
    // ************************************************************************

    hd_key_lineage lineage = hd_key_lineage{ lineage_.testnet,
        static_cast<uint8_t>(lineage_.depth + 1), fingerprint(), i };

    return hd_public_key(out, I.R, lineage);
}

BCW_API hd_private_key::hd_private_key()
  : hd_public_key()
{
}

BCW_API hd_private_key::hd_private_key(const secret_parameter& private_key,
    const chain_code_type& chain_code, hd_key_lineage lineage)
  : hd_public_key(secret_to_public_key(private_key), chain_code, lineage),
    k_(private_key)
{
}

BCW_API hd_private_key::hd_private_key(const data_chunk& seed, bool testnet)
  : hd_public_key()
{
    std::string key("Bitcoin seed");
    split_long_hash I = split(hmac_sha512_hash(seed, to_data_chunk(key)));
    ssl_bignum il = BN_bin2bn(I.L.data(), (int)I.L.size(), nullptr);

    // ************************************************************************
    ssl_bignum n = BN_bin2bn(secp256k1_n.data(), (int)secp256k1_n.size(),
        nullptr);
    // ************************************************************************

    // The key is invalid if parse256(IL) >= n or 0:
    if (0 <= BN_cmp(il, n) || BN_is_zero(il.ptr))
        return;

    hd_key_lineage null_seed = hd_key_lineage{ testnet, 0, { { 0 } }, 0 };

    *this = hd_private_key(I.L, I.R, null_seed);
}

BCW_API const secret_parameter& hd_private_key::private_key() const
{
    return k_;
}

BCW_API std::string hd_private_key::serialize() const
{
    data_chunk data;
    data.reserve(4 + 1 + 4 + 4 + 32 + 33 + 4);

    extend_data(data, ser32(lineage_.testnet ? testnet_private_prefix :
        mainnet_private_prefix));
    data.push_back(lineage_.depth);
    extend_data(data, lineage_.parent_fingerprint);
    extend_data(data, ser32(lineage_.child_number));
    extend_data(data, c_);
    data.push_back(0x00);
    extend_data(data, k_);

    extend_data(data, uncast_type(bitcoin_checksum(data)));
    return encode_base58(data);
}

BCW_API hd_private_key hd_private_key::generate_private_key(uint32_t i)
{
    if (!valid_)
        return hd_private_key();

    data_chunk data;
    data.reserve(33 + 4);
    if (first_hardened_key <= i)
    {
        data.push_back(0x00);
        extend_data(data, k_);
        extend_data(data, ser32(i));
    }
    else
    {
        extend_data(data, K_);
        extend_data(data, ser32(i));
    }
    auto I = split(hmac_sha512_hash(data, to_data_chunk(c_)));

    // The child key ki is (parse256(IL) + kpar) mod n:
    ssl_bn_ctx ctx = BN_CTX_new();
    ssl_bignum ki = BN_new();
    ssl_bignum kpar = BN_bin2bn(k_.data(), (int)k_.size(), nullptr);
    ssl_bignum il = BN_bin2bn(I.L.data(), (int)I.L.size(), nullptr);

    // ************************************************************************
    ssl_bignum n = BN_bin2bn(secp256k1_n.data(), (int)secp256k1_n.size(),
        nullptr);
    // ************************************************************************

    if (!ctx || !ki || !kpar || !il || !n)
        return hd_private_key();

    if (!BN_mod_add(ki, kpar, il, n, ctx))
        return hd_private_key();

    // The key is invalid if parse256(IL) >= n or ki == 0:
    if (0 <= BN_cmp(il, n) || BN_is_zero(ki.ptr))
        return hd_private_key();

    secret_parameter out{ { 0 } };
    int ki_size = BN_num_bytes(ki);
    if (ki_size != BN_bn2bin(ki, &out[out.size() - ki_size]))
        return hd_private_key();

    hd_key_lineage lineage = hd_key_lineage
    {
        lineage_.testnet,
        static_cast<uint8_t>(lineage_.depth + 1),
        fingerprint(),
        i
    };

    return hd_private_key(out, I.R, lineage);
}

BCW_API hd_public_key hd_private_key::generate_public_key(uint32_t i)
{
    return generate_private_key(i);
}

} // libwallet

