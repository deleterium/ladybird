/*
 * Copyright (c) 2023, Michiel Visser <opensource@webmichiel.nl>
 * Copyright (c) 2024-2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Endian.h>
#include <AK/Error.h>
#include <AK/MemoryStream.h>
#include <AK/StdLibExtras.h>
#include <AK/StringView.h>
#include <AK/UFixedBigInt.h>
#include <AK/UFixedBigIntDivision.h>
#include <LibCrypto/ASN1/Constants.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/Curves/EllipticCurve.h>
#include <LibCrypto/OpenSSL.h>

#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

namespace {
// Used by ASN1 macros
static String s_error_string;
}

namespace Crypto::Curves {

struct SECPxxxr1CurveParameters {
    char const* name;
    StringView prime;
    StringView a;
    StringView b;
    StringView order;
    StringView generator_point;
};

struct SECPxxxr1Point {
    UnsignedBigInteger x;
    UnsignedBigInteger y;
    size_t size;

    static ErrorOr<ByteBuffer> scalar_to_bytes(UnsignedBigInteger const& a, size_t size)
    {
        auto a_bytes = TRY(ByteBuffer::create_uninitialized(a.byte_length()));
        auto a_size = a.export_data(a_bytes.span());
        VERIFY(a_size >= size);

        for (size_t i = 0; i < a_size - size; i++) {
            if (a_bytes[i] != 0) {
                return Error::from_string_literal("Scalar is too large for the given size");
            }
        }

        return a_bytes.slice(a_size - size, size);
    }

    static ErrorOr<SECPxxxr1Point> from_uncompressed(ReadonlyBytes data)
    {
        if (data.size() < 1 || data[0] != 0x04)
            return Error::from_string_literal("Invalid length or not an uncompressed SECPxxxr1 point");

        auto half_size = (data.size() - 1) / 2;
        return SECPxxxr1Point {
            UnsignedBigInteger::import_data(data.slice(1, half_size)),
            UnsignedBigInteger::import_data(data.slice(1 + half_size, half_size)),
            half_size,
        };
    }

    ErrorOr<ByteBuffer> x_bytes() const
    {
        return scalar_to_bytes(x, size);
    }

    ErrorOr<ByteBuffer> y_bytes() const
    {
        return scalar_to_bytes(y, size);
    }

    ErrorOr<ByteBuffer> to_uncompressed() const
    {
        auto x = TRY(x_bytes());
        auto y = TRY(y_bytes());

        auto bytes = TRY(ByteBuffer::create_uninitialized(1 + (size * 2)));
        bytes[0] = 0x04; // uncompressed
        bytes.overwrite(1, x.data(), size);
        bytes.overwrite(1 + size, y.data(), size);
        return bytes;
    }
};

struct SECPxxxr1Signature {
    UnsignedBigInteger r;
    UnsignedBigInteger s;
    size_t size;

    static ErrorOr<SECPxxxr1Signature> from_asn(Span<int const> curve_oid, ReadonlyBytes signature, Vector<StringView> current_scope)
    {
        ASN1::Decoder decoder(signature);
        ENTER_TYPED_SCOPE(Sequence, "SECPxxxr1Signature");
        READ_OBJECT(Integer, UnsignedBigInteger, r_big_int);
        READ_OBJECT(Integer, UnsignedBigInteger, s_big_int);

        size_t scalar_size;
        if (curve_oid == ASN1::secp256r1_oid) {
            scalar_size = ceil_div(256, 8);
        } else if (curve_oid == ASN1::secp384r1_oid) {
            scalar_size = ceil_div(384, 8);
        } else if (curve_oid == ASN1::secp521r1_oid) {
            scalar_size = ceil_div(521, 8);
        } else {
            return Error::from_string_literal("Unknown SECPxxxr1 curve");
        }

        if (r_big_int.byte_length() < scalar_size || s_big_int.byte_length() < scalar_size)
            return Error::from_string_literal("Invalid SECPxxxr1 signature");

        return SECPxxxr1Signature { r_big_int, s_big_int, scalar_size };
    }

    ErrorOr<ByteBuffer> r_bytes() const
    {
        return SECPxxxr1Point::scalar_to_bytes(r, size);
    }

    ErrorOr<ByteBuffer> s_bytes() const
    {
        return SECPxxxr1Point::scalar_to_bytes(s, size);
    }

    ErrorOr<ByteBuffer> to_asn()
    {
        ASN1::Encoder encoder;
        TRY(encoder.write_constructed(ASN1::Class::Universal, ASN1::Kind::Sequence, [&]() -> ErrorOr<void> {
            TRY(encoder.write(r));
            TRY(encoder.write(s));
            return {};
        }));

        return encoder.finish();
    }
};

template<size_t bit_size, SECPxxxr1CurveParameters const& CURVE_PARAMETERS>
class SECPxxxr1 : public EllipticCurve {
private:
    using StorageType = AK::UFixedBigInt<bit_size>;
    using StorageTypeX2 = AK::UFixedBigInt<bit_size * 2>;

    struct JacobianPoint {
        StorageType x;
        StorageType y;
        StorageType z;
    };

    // Curve parameters
    static constexpr size_t KEY_BIT_SIZE = bit_size;
    static constexpr size_t KEY_BYTE_SIZE = ceil_div(KEY_BIT_SIZE, 8ull);
    static constexpr size_t POINT_BYTE_SIZE = 1 + 2 * KEY_BYTE_SIZE;

    static constexpr StorageType make_unsigned_fixed_big_int_from_string(StringView str)
    {
        StorageType result { 0 };
        for (auto c : str) {
            if (c == '_')
                continue;

            result <<= 4;
            result |= parse_ascii_hex_digit(c);
        }
        return result;
    }

    static constexpr StorageType PRIME = make_unsigned_fixed_big_int_from_string(CURVE_PARAMETERS.prime);
    static constexpr StorageType A = make_unsigned_fixed_big_int_from_string(CURVE_PARAMETERS.a);
    static constexpr StorageType B = make_unsigned_fixed_big_int_from_string(CURVE_PARAMETERS.b);
    static constexpr StorageType ORDER = make_unsigned_fixed_big_int_from_string(CURVE_PARAMETERS.order);

    static constexpr Array<u8, POINT_BYTE_SIZE> make_generator_point_bytes(StringView generator_point)
    {
        Array<u8, POINT_BYTE_SIZE> buf_array { 0 };

        auto it = generator_point.begin();
        for (size_t i = 0; i < POINT_BYTE_SIZE; i++) {
            if (it == CURVE_PARAMETERS.generator_point.end())
                break;

            while (*it == '_') {
                it++;
            }

            buf_array[i] = parse_ascii_hex_digit(*it) * 16;
            it++;
            if (it == CURVE_PARAMETERS.generator_point.end())
                break;

            buf_array[i] += parse_ascii_hex_digit(*it);
            it++;
        }

        return buf_array;
    }

    static constexpr Array<u8, POINT_BYTE_SIZE> GENERATOR_POINT = make_generator_point_bytes(CURVE_PARAMETERS.generator_point);

    // Check that the generator point starts with 0x04
    static_assert(GENERATOR_POINT[0] == 0x04);

    static constexpr StorageType calculate_modular_inverse_mod_r(StorageType value)
    {
        // Calculate the modular multiplicative inverse of value mod 2^bit_size using the extended euclidean algorithm
        using StorageTypeP1 = AK::UFixedBigInt<bit_size + 1>;

        StorageTypeP1 old_r = value;
        StorageTypeP1 r = static_cast<StorageTypeP1>(1u) << KEY_BIT_SIZE;
        StorageTypeP1 old_s = 1u;
        StorageTypeP1 s = 0u;

        while (!r.is_zero_constant_time()) {
            StorageTypeP1 r_save = r;
            StorageTypeP1 quotient = old_r.div_mod(r, r);
            old_r = r_save;

            StorageTypeP1 s_save = s;
            s = old_s - quotient * s;
            old_s = s_save;
        }

        return static_cast<StorageType>(old_s);
    }

    static constexpr StorageType calculate_r2_mod(StorageType modulus)
    {
        // Calculate the value of R^2 mod modulus, where R = 2^bit_size
        using StorageTypeX2P1 = AK::UFixedBigInt<bit_size * 2 + 1>;

        StorageTypeX2P1 r2 = static_cast<StorageTypeX2P1>(1u) << (2 * KEY_BIT_SIZE);
        return r2 % modulus;
    }

    // Verify that A = -3 mod p, which is required for some optimizations
    static_assert(A == PRIME - 3);

    // Precomputed helper values for reduction and Montgomery multiplication
    static constexpr StorageType REDUCE_PRIME = StorageType { 0 } - PRIME;
    static constexpr StorageType REDUCE_ORDER = StorageType { 0 } - ORDER;
    static constexpr StorageType PRIME_INVERSE_MOD_R = StorageType { 0 } - calculate_modular_inverse_mod_r(PRIME);
    static constexpr StorageType ORDER_INVERSE_MOD_R = StorageType { 0 } - calculate_modular_inverse_mod_r(ORDER);
    static constexpr StorageType R2_MOD_PRIME = calculate_r2_mod(PRIME);
    static constexpr StorageType R2_MOD_ORDER = calculate_r2_mod(ORDER);

public:
    size_t key_size() override { return POINT_BYTE_SIZE; }

    ErrorOr<ByteBuffer> generate_private_key() override
    {
        auto key = TRY(generate_private_key_scalar());

        auto buffer = TRY(ByteBuffer::create_uninitialized(KEY_BYTE_SIZE));
        auto buffer_bytes = buffer.bytes();
        auto size = key.export_data(buffer_bytes);
        return buffer.slice(0, size);
    }

    ErrorOr<UnsignedBigInteger> generate_private_key_scalar()
    {
        auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", CURVE_PARAMETERS.name)));

        auto priv_bn = TRY(OpenSSL_BN::create());
        auto* priv_bn_ptr = priv_bn.ptr();
        OPENSSL_TRY(EVP_PKEY_get_bn_param(key.ptr(), OSSL_PKEY_PARAM_PRIV_KEY, &priv_bn_ptr));

        return TRY(openssl_bignum_to_unsigned_big_integer(priv_bn));
    }

    ErrorOr<ByteBuffer> generate_public_key(ReadonlyBytes a) override
    {
        auto a_int = UnsignedBigInteger::import_data(a);
        auto point = TRY(generate_public_key_point(a_int));
        return point.to_uncompressed();
    }

    ErrorOr<SECPxxxr1Point> generate_public_key_point(UnsignedBigInteger scalar)
    {
        auto* group = EC_GROUP_new_by_curve_name(EC_curve_nist2nid(CURVE_PARAMETERS.name));
        ScopeGuard const free_group = [&] { EC_GROUP_free(group); };

        auto scalar_int = TRY(unsigned_big_integer_to_openssl_bignum(scalar));

        auto* r = EC_POINT_new(group);
        ScopeGuard const free_r = [&] { EC_POINT_free(r); };

        OPENSSL_TRY(EC_POINT_mul(group, r, scalar_int.ptr(), nullptr, nullptr, nullptr));

        auto x = TRY(OpenSSL_BN::create());
        auto y = TRY(OpenSSL_BN::create());

        OPENSSL_TRY(EC_POINT_get_affine_coordinates(group, r, x.ptr(), y.ptr(), nullptr));

        return SECPxxxr1Point {
            TRY(openssl_bignum_to_unsigned_big_integer(x)),
            TRY(openssl_bignum_to_unsigned_big_integer(y)),
            KEY_BYTE_SIZE,
        };
    }

    ErrorOr<ByteBuffer> compute_coordinate(ReadonlyBytes scalar_bytes, ReadonlyBytes point_bytes) override
    {
        auto scalar = UnsignedBigInteger::import_data(scalar_bytes);
        auto point = TRY(SECPxxxr1Point::from_uncompressed(point_bytes));
        auto result = TRY(compute_coordinate_point(scalar, { point.x, point.y, KEY_BYTE_SIZE }));
        return result.to_uncompressed();
    }

    ErrorOr<SECPxxxr1Point> compute_coordinate_point(UnsignedBigInteger scalar, SECPxxxr1Point point)
    {
        auto* group = EC_GROUP_new_by_curve_name(EC_curve_nist2nid(CURVE_PARAMETERS.name));
        ScopeGuard const free_group = [&] { EC_GROUP_free(group); };

        auto scalar_int = TRY(unsigned_big_integer_to_openssl_bignum(scalar));

        auto qx = TRY(unsigned_big_integer_to_openssl_bignum(point.x));
        auto qy = TRY(unsigned_big_integer_to_openssl_bignum(point.y));

        auto* q = EC_POINT_new(group);
        ScopeGuard const free_q = [&] { EC_POINT_free(q); };

        OPENSSL_TRY(EC_POINT_set_affine_coordinates(group, q, qx.ptr(), qy.ptr(), nullptr));

        auto* r = EC_POINT_new(group);
        ScopeGuard const free_r = [&] { EC_POINT_free(r); };

        OPENSSL_TRY(EC_POINT_mul(group, r, nullptr, q, scalar_int.ptr(), nullptr));

        auto rx = TRY(OpenSSL_BN::create());
        auto ry = TRY(OpenSSL_BN::create());

        OPENSSL_TRY(EC_POINT_get_affine_coordinates(group, r, rx.ptr(), ry.ptr(), nullptr));

        return SECPxxxr1Point {
            TRY(openssl_bignum_to_unsigned_big_integer(rx)),
            TRY(openssl_bignum_to_unsigned_big_integer(ry)),
            KEY_BYTE_SIZE,
        };
    }

    ErrorOr<ByteBuffer> derive_premaster_key(ReadonlyBytes shared_point_bytes) override
    {
        auto shared_point = TRY(SECPxxxr1Point::from_uncompressed(shared_point_bytes));
        auto premaster_key_point = TRY(derive_premaster_key_point(shared_point));
        return premaster_key_point.to_uncompressed();
    }

    ErrorOr<SECPxxxr1Point> derive_premaster_key_point(SECPxxxr1Point shared_point)
    {
        return shared_point;
    }

    ErrorOr<bool> verify_point(ReadonlyBytes hash, SECPxxxr1Point pubkey, SECPxxxr1Signature signature)
    {
        auto ctx_import = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr)));

        OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx_import.ptr()));

        auto* params_bld = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
        ScopeGuard const free_params_bld = [&] { OSSL_PARAM_BLD_free(params_bld); };

        OPENSSL_TRY(OSSL_PARAM_BLD_push_utf8_string(params_bld, OSSL_PKEY_PARAM_GROUP_NAME, CURVE_PARAMETERS.name, strlen(CURVE_PARAMETERS.name)));

        auto pubkey_bytes = TRY(pubkey.to_uncompressed());
        OPENSSL_TRY(OSSL_PARAM_BLD_push_octet_string(params_bld, OSSL_PKEY_PARAM_PUB_KEY, pubkey_bytes.data(), pubkey_bytes.size()));

        auto* params = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_to_param(params_bld));
        ScopeGuard const free_params = [&] { OSSL_PARAM_free(params); };

        auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new()));
        auto* key_ptr = key.ptr();
        OPENSSL_TRY(EVP_PKEY_fromdata(ctx_import.ptr(), &key_ptr, EVP_PKEY_PUBLIC_KEY, params));

        auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, key.ptr(), nullptr)));

        OPENSSL_TRY(EVP_PKEY_verify_init(ctx.ptr()));

        auto* sig_obj = OPENSSL_TRY_PTR(ECDSA_SIG_new());
        ScopeGuard const free_sig_obj = [&] { ECDSA_SIG_free(sig_obj); };

        auto r = TRY(unsigned_big_integer_to_openssl_bignum(signature.r));
        auto s = TRY(unsigned_big_integer_to_openssl_bignum(signature.s));

        // Let sig_obj own a copy of r and s
        OPENSSL_TRY(ECDSA_SIG_set0(sig_obj, BN_dup(r.ptr()), BN_dup(s.ptr())));

        u8* sig = nullptr;
        ScopeGuard const free_sig = [&] { OPENSSL_free(sig); };

        auto sig_len = TRY([&] -> ErrorOr<int> {
            auto ret = i2d_ECDSA_SIG(sig_obj, &sig);
            if (ret <= 0) {
                OPENSSL_TRY(ret);
                VERIFY_NOT_REACHED();
            }
            return ret;
        }());

        auto ret = EVP_PKEY_verify(ctx.ptr(), sig, sig_len, hash.data(), hash.size());
        if (ret == 1)
            return true;
        if (ret == 0)
            return false;
        OPENSSL_TRY(ret);
        VERIFY_NOT_REACHED();
    }

    ErrorOr<bool> verify(ReadonlyBytes hash, ReadonlyBytes pubkey, SECPxxxr1Signature signature)
    {
        auto pubkey_point = TRY(SECPxxxr1Point::from_uncompressed(pubkey));
        return verify_point(hash, pubkey_point, signature);
    }

    ErrorOr<SECPxxxr1Signature> sign_scalar(ReadonlyBytes hash, UnsignedBigInteger private_key)
    {
        auto ctx_import = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr)));

        OPENSSL_TRY(EVP_PKEY_fromdata_init(ctx_import.ptr()));

        auto d = TRY(unsigned_big_integer_to_openssl_bignum(private_key));

        auto* params_bld = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_new());
        ScopeGuard const free_params_bld = [&] { OSSL_PARAM_BLD_free(params_bld); };

        OPENSSL_TRY(OSSL_PARAM_BLD_push_utf8_string(params_bld, OSSL_PKEY_PARAM_GROUP_NAME, CURVE_PARAMETERS.name, strlen(CURVE_PARAMETERS.name)));
        OPENSSL_TRY(OSSL_PARAM_BLD_push_BN(params_bld, OSSL_PKEY_PARAM_PRIV_KEY, d.ptr()));

        auto* params = OPENSSL_TRY_PTR(OSSL_PARAM_BLD_to_param(params_bld));
        ScopeGuard const free_params = [&] { OSSL_PARAM_free(params); };

        auto key = TRY(OpenSSL_PKEY::wrap(EVP_PKEY_new()));
        auto* key_ptr = key.ptr();
        OPENSSL_TRY(EVP_PKEY_fromdata(ctx_import.ptr(), &key_ptr, EVP_PKEY_KEYPAIR, params));

        auto ctx = TRY(OpenSSL_PKEY_CTX::wrap(EVP_PKEY_CTX_new_from_pkey(nullptr, key.ptr(), nullptr)));

        OPENSSL_TRY(EVP_PKEY_sign_init(ctx.ptr()));

        size_t sig_len = 0;
        OPENSSL_TRY(EVP_PKEY_sign(ctx.ptr(), nullptr, &sig_len, hash.data(), hash.size()));

        auto sig = TRY(ByteBuffer::create_uninitialized(sig_len));
        OPENSSL_TRY(EVP_PKEY_sign(ctx.ptr(), sig.data(), &sig_len, hash.data(), hash.size()));

        auto const* sig_data = sig.data();
        auto* sig_obj = OPENSSL_TRY_PTR(d2i_ECDSA_SIG(nullptr, &sig_data, sig.size()));
        ScopeGuard const free_sig_obj = [&] { ECDSA_SIG_free(sig_obj); };

        // Duplicate r and s so that sig_obj can own them
        auto r = TRY(OpenSSL_BN::wrap(BN_dup(OPENSSL_TRY_PTR(ECDSA_SIG_get0_r(sig_obj)))));
        auto s = TRY(OpenSSL_BN::wrap(BN_dup(OPENSSL_TRY_PTR(ECDSA_SIG_get0_s(sig_obj)))));

        return SECPxxxr1Signature {
            TRY(openssl_bignum_to_unsigned_big_integer(r)),
            TRY(openssl_bignum_to_unsigned_big_integer(s)),
            KEY_BYTE_SIZE,
        };
    }

    ErrorOr<SECPxxxr1Signature> sign(ReadonlyBytes hash, ReadonlyBytes private_key_bytes)
    {
        auto signature = TRY(sign_scalar(hash, UnsignedBigInteger::import_data(private_key_bytes.data(), private_key_bytes.size())));
        return signature;
    }

private:
    StorageType unsigned_big_integer_to_storage_type(UnsignedBigInteger big)
    {
        constexpr size_t word_count = (KEY_BYTE_SIZE + 4 - 1) / 4;
        VERIFY(big.length() >= word_count);

        StorageType val = 0u;
        for (size_t i = 0; i < word_count; i++) {
            StorageType rr = big.words()[i];
            val |= (rr << (i * 32));
        }
        return val;
    }

    UnsignedBigInteger storage_type_to_unsigned_big_integer(StorageType val)
    {
        constexpr size_t word_count = (KEY_BYTE_SIZE + 4 - 1) / 4;
        Vector<UnsignedBigInteger::Word, word_count> words;
        for (size_t i = 0; i < word_count; i++) {
            words.append(static_cast<UnsignedBigInteger::Word>((val >> (i * 32)) & 0xFFFFFFFF));
        }
        return UnsignedBigInteger(move(words));
    }

    ErrorOr<JacobianPoint> generate_public_key_internal(StorageType a)
    {
        AK::FixedMemoryStream generator_point_stream { GENERATOR_POINT };
        JacobianPoint point = TRY(read_uncompressed_point(generator_point_stream));
        return compute_coordinate_internal(a, point);
    }

    ErrorOr<JacobianPoint> compute_coordinate_internal(StorageType scalar, JacobianPoint point)
    {
        // FIXME: This will slightly bias the distribution of client secrets
        scalar = modular_reduce_order(scalar);
        if (scalar.is_zero_constant_time())
            return Error::from_string_literal("SECPxxxr1: scalar is zero");

        // Convert the input point into Montgomery form
        point.x = to_montgomery(point.x);
        point.y = to_montgomery(point.y);
        point.z = to_montgomery(point.z);

        // Check that the point is on the curve
        if (!is_point_on_curve(point))
            return Error::from_string_literal("SECPxxxr1: point is not on the curve");

        JacobianPoint result { 0, 0, 0 };
        JacobianPoint temp_result { 0, 0, 0 };

        // Calculate the scalar times point multiplication in constant time
        for (size_t i = 0; i < KEY_BIT_SIZE; i++) {
            temp_result = point_add(result, point);

            auto condition = (scalar & 1u) == 1u;
            result.x = select(result.x, temp_result.x, condition);
            result.y = select(result.y, temp_result.y, condition);
            result.z = select(result.z, temp_result.z, condition);

            point = point_double(point);
            scalar >>= 1u;
        }

        // Convert from Jacobian coordinates back to Affine coordinates
        convert_jacobian_to_affine(result);

        // Make sure the resulting point is on the curve
        VERIFY(is_point_on_curve(result));

        // Convert the result back from Montgomery form
        result.x = from_montgomery(result.x);
        result.y = from_montgomery(result.y);
        result.z = from_montgomery(result.z);
        // Final modular reduction on the coordinates
        result.x = modular_reduce(result.x);
        result.y = modular_reduce(result.y);
        result.z = modular_reduce(result.z);

        return result;
    }

    static ErrorOr<JacobianPoint> read_uncompressed_point(Stream& stream)
    {
        // Make sure the point is uncompressed
        if (TRY(stream.read_value<u8>()) != 0x04)
            return Error::from_string_literal("SECPxxxr1: point is not uncompressed format");

        JacobianPoint point {
            TRY(stream.read_value<BigEndian<StorageType>>()),
            TRY(stream.read_value<BigEndian<StorageType>>()),
            1u,
        };

        return point;
    }

    constexpr StorageType select(StorageType const& left, StorageType const& right, bool condition)
    {
        // If condition = 0 return left else right
        StorageType mask = static_cast<StorageType>(condition) - 1;
        AK::taint_for_optimizer(mask);

        return (left & mask) | (right & ~mask);
    }

    constexpr StorageType modular_reduce(StorageType const& value)
    {
        // Add -prime % 2^KEY_BIT_SIZE
        bool carry = false;
        StorageType other = value.addc(REDUCE_PRIME, carry);

        // Check for overflow
        return select(value, other, carry);
    }

    constexpr StorageType modular_reduce_order(StorageType const& value)
    {
        // Add -order % 2^KEY_BIT_SIZE
        bool carry = false;
        StorageType other = value.addc(REDUCE_ORDER, carry);

        // Check for overflow
        return select(value, other, carry);
    }

    constexpr StorageType modular_add(StorageType const& left, StorageType const& right, bool carry_in = false)
    {
        bool carry = carry_in;
        StorageType output = left.addc(right, carry);

        // If there is a carry, subtract p by adding 2^KEY_BIT_SIZE - p
        StorageType addend = select(0u, REDUCE_PRIME, carry);
        carry = false;
        output = output.addc(addend, carry);

        // If there is still a carry, subtract p by adding 2^KEY_BIT_SIZE - p
        addend = select(0u, REDUCE_PRIME, carry);
        return output + addend;
    }

    constexpr StorageType modular_sub(StorageType const& left, StorageType const& right)
    {
        bool borrow = false;
        StorageType output = left.subc(right, borrow);

        // If there is a borrow, add p by subtracting 2^KEY_BIT_SIZE - p
        StorageType sub = select(0u, REDUCE_PRIME, borrow);
        borrow = false;
        output = output.subc(sub, borrow);

        // If there is still a borrow, add p by subtracting 2^KEY_BIT_SIZE - p
        sub = select(0u, REDUCE_PRIME, borrow);
        return output - sub;
    }

    constexpr StorageType modular_multiply(StorageType const& left, StorageType const& right)
    {
        // Modular multiplication using the Montgomery method: https://en.wikipedia.org/wiki/Montgomery_modular_multiplication
        // This requires that the inputs to this function are in Montgomery form.

        // T = left * right
        StorageTypeX2 mult = left.wide_multiply(right);
        StorageType mult_mod_r = static_cast<StorageType>(mult);

        // m = ((T mod R) * curve_p')
        StorageType m = mult_mod_r * PRIME_INVERSE_MOD_R;

        // mp = (m mod R) * curve_p
        StorageTypeX2 mp = m.wide_multiply(PRIME);

        // t = (T + mp)
        bool carry = false;
        mult_mod_r.addc(static_cast<StorageType>(mp), carry);

        // output = t / R
        StorageType mult_high = static_cast<StorageType>(mult >> KEY_BIT_SIZE);
        StorageType mp_high = static_cast<StorageType>(mp >> KEY_BIT_SIZE);
        return modular_add(mult_high, mp_high, carry);
    }

    constexpr StorageType modular_square(StorageType const& value)
    {
        return modular_multiply(value, value);
    }

    constexpr StorageType to_montgomery(StorageType const& value)
    {
        return modular_multiply(value, R2_MOD_PRIME);
    }

    constexpr StorageType from_montgomery(StorageType const& value)
    {
        return modular_multiply(value, 1u);
    }

    constexpr StorageType modular_inverse(StorageType const& value)
    {
        // Modular inverse modulo the curve prime can be computed using Fermat's little theorem: a^(p-2) mod p = a^-1 mod p.
        // Calculating a^(p-2) mod p can be done using the square-and-multiply exponentiation method, as p-2 is constant.
        StorageType base = value;
        StorageType result = to_montgomery(1u);
        StorageType prime_minus_2 = PRIME - 2u;

        for (size_t i = 0; i < KEY_BIT_SIZE; i++) {
            if ((prime_minus_2 & 1u) == 1u) {
                result = modular_multiply(result, base);
            }
            base = modular_square(base);
            prime_minus_2 >>= 1u;
        }

        return result;
    }

    constexpr StorageType modular_add_order(StorageType const& left, StorageType const& right, bool carry_in = false)
    {
        bool carry = carry_in;
        StorageType output = left.addc(right, carry);

        // If there is a carry, subtract n by adding 2^KEY_BIT_SIZE - n
        StorageType addend = select(0u, REDUCE_ORDER, carry);
        carry = false;
        output = output.addc(addend, carry);

        // If there is still a carry, subtract n by adding 2^KEY_BIT_SIZE - n
        addend = select(0u, REDUCE_ORDER, carry);
        return output + addend;
    }

    constexpr StorageType modular_multiply_order(StorageType const& left, StorageType const& right)
    {
        // Modular multiplication using the Montgomery method: https://en.wikipedia.org/wiki/Montgomery_modular_multiplication
        // This requires that the inputs to this function are in Montgomery form.

        // T = left * right
        StorageTypeX2 mult = left.wide_multiply(right);
        StorageType mult_mod_r = static_cast<StorageType>(mult);

        // m = ((T mod R) * curve_n')
        StorageType m = mult_mod_r * ORDER_INVERSE_MOD_R;

        // mp = (m mod R) * curve_n
        StorageTypeX2 mp = m.wide_multiply(ORDER);

        // t = (T + mp)
        bool carry = false;
        mult_mod_r.addc(static_cast<StorageType>(mp), carry);

        // output = t / R
        StorageType mult_high = static_cast<StorageType>(mult >> KEY_BIT_SIZE);
        StorageType mp_high = static_cast<StorageType>(mp >> KEY_BIT_SIZE);
        return modular_add_order(mult_high, mp_high, carry);
    }

    constexpr StorageType modular_square_order(StorageType const& value)
    {
        return modular_multiply_order(value, value);
    }

    constexpr StorageType to_montgomery_order(StorageType const& value)
    {
        return modular_multiply_order(value, R2_MOD_ORDER);
    }

    constexpr StorageType from_montgomery_order(StorageType const& value)
    {
        return modular_multiply_order(value, 1u);
    }

    constexpr StorageType modular_inverse_order(StorageType const& value)
    {
        // Modular inverse modulo the curve order can be computed using Fermat's little theorem: a^(n-2) mod n = a^-1 mod n.
        // Calculating a^(n-2) mod n can be done using the square-and-multiply exponentiation method, as n-2 is constant.
        StorageType base = value;
        StorageType result = to_montgomery_order(1u);
        StorageType order_minus_2 = ORDER - 2u;

        for (size_t i = 0; i < KEY_BIT_SIZE; i++) {
            if ((order_minus_2 & 1u) == 1u) {
                result = modular_multiply_order(result, base);
            }
            base = modular_square_order(base);
            order_minus_2 >>= 1u;
        }

        return result;
    }

    JacobianPoint point_double(JacobianPoint const& point)
    {
        // Based on "Point Doubling" from http://point-at-infinity.org/ecc/Prime_Curve_Jacobian_Coordinates.html

        // if (Y == 0)
        //   return POINT_AT_INFINITY
        if (point.y.is_zero_constant_time()) {
            VERIFY_NOT_REACHED();
        }

        StorageType temp;

        // Y2 = Y^2
        StorageType y2 = modular_square(point.y);

        // S = 4*X*Y2
        StorageType s = modular_multiply(point.x, y2);
        s = modular_add(s, s);
        s = modular_add(s, s);

        // M = 3*X^2 + a*Z^4 = 3*(X + Z^2)*(X - Z^2)
        // This specific equation from https://github.com/earlephilhower/bearssl-esp8266/blob/6105635531027f5b298aa656d44be2289b2d434f/src/ec/ec_p256_m64.c#L811-L816
        // This simplification only works because a = -3 mod p
        temp = modular_square(point.z);
        StorageType m = modular_add(point.x, temp);
        temp = modular_sub(point.x, temp);
        m = modular_multiply(m, temp);
        temp = modular_add(m, m);
        m = modular_add(m, temp);

        // X' = M^2 - 2*S
        StorageType xp = modular_square(m);
        xp = modular_sub(xp, s);
        xp = modular_sub(xp, s);

        // Y' = M*(S - X') - 8*Y2^2
        StorageType yp = modular_sub(s, xp);
        yp = modular_multiply(yp, m);
        temp = modular_square(y2);
        temp = modular_add(temp, temp);
        temp = modular_add(temp, temp);
        temp = modular_add(temp, temp);
        yp = modular_sub(yp, temp);

        // Z' = 2*Y*Z
        StorageType zp = modular_multiply(point.y, point.z);
        zp = modular_add(zp, zp);

        // return (X', Y', Z')
        return JacobianPoint { xp, yp, zp };
    }

    JacobianPoint point_add(JacobianPoint const& point_a, JacobianPoint const& point_b)
    {
        // Based on "Point Addition" from  http://point-at-infinity.org/ecc/Prime_Curve_Jacobian_Coordinates.html
        if (point_a.x.is_zero_constant_time() && point_a.y.is_zero_constant_time() && point_a.z.is_zero_constant_time()) {
            return point_b;
        }

        StorageType temp;

        temp = modular_square(point_b.z);
        // U1 = X1*Z2^2
        StorageType u1 = modular_multiply(point_a.x, temp);
        // S1 = Y1*Z2^3
        StorageType s1 = modular_multiply(point_a.y, temp);
        s1 = modular_multiply(s1, point_b.z);

        temp = modular_square(point_a.z);
        // U2 = X2*Z1^2
        StorageType u2 = modular_multiply(point_b.x, temp);
        // S2 = Y2*Z1^3
        StorageType s2 = modular_multiply(point_b.y, temp);
        s2 = modular_multiply(s2, point_a.z);

        // if (U1 == U2)
        //   if (S1 != S2)
        //     return POINT_AT_INFINITY
        //   else
        //     return POINT_DOUBLE(X1, Y1, Z1)
        if (u1.is_equal_to_constant_time(u2)) {
            if (s1.is_equal_to_constant_time(s2)) {
                return point_double(point_a);
            } else {
                VERIFY_NOT_REACHED();
            }
        }

        // H = U2 - U1
        StorageType h = modular_sub(u2, u1);
        StorageType h2 = modular_square(h);
        StorageType h3 = modular_multiply(h2, h);
        // R = S2 - S1
        StorageType r = modular_sub(s2, s1);
        // X3 = R^2 - H^3 - 2*U1*H^2
        StorageType x3 = modular_square(r);
        x3 = modular_sub(x3, h3);
        temp = modular_multiply(u1, h2);
        temp = modular_add(temp, temp);
        x3 = modular_sub(x3, temp);
        // Y3 = R*(U1*H^2 - X3) - S1*H^3
        StorageType y3 = modular_multiply(u1, h2);
        y3 = modular_sub(y3, x3);
        y3 = modular_multiply(y3, r);
        temp = modular_multiply(s1, h3);
        y3 = modular_sub(y3, temp);
        // Z3 = H*Z1*Z2
        StorageType z3 = modular_multiply(h, point_a.z);
        z3 = modular_multiply(z3, point_b.z);
        // return (X3, Y3, Z3)
        return JacobianPoint { x3, y3, z3 };
    }

    void convert_jacobian_to_affine(JacobianPoint& point)
    {
        StorageType temp;
        // X' = X/Z^2
        temp = modular_square(point.z);
        temp = modular_inverse(temp);
        point.x = modular_multiply(point.x, temp);
        // Y' = Y/Z^3
        temp = modular_square(point.z);
        temp = modular_multiply(temp, point.z);
        temp = modular_inverse(temp);
        point.y = modular_multiply(point.y, temp);
        // Z' = 1
        point.z = to_montgomery(1u);
    }

    bool is_point_on_curve(JacobianPoint const& point)
    {
        // This check requires the point to be in Montgomery form, with Z=1
        StorageType temp, temp2;

        // Calulcate Y^2 - X^3 - a*X - b = Y^2 - X^3 + 3*X - b
        temp = modular_square(point.y);
        temp2 = modular_square(point.x);
        temp2 = modular_multiply(temp2, point.x);
        temp = modular_sub(temp, temp2);
        temp = modular_add(temp, point.x);
        temp = modular_add(temp, point.x);
        temp = modular_add(temp, point.x);
        temp = modular_sub(temp, to_montgomery(B));
        temp = modular_reduce(temp);

        return temp.is_zero_constant_time() && point.z.is_equal_to_constant_time(to_montgomery(1u));
    }
};

// SECP256r1 curve
static constexpr SECPxxxr1CurveParameters SECP256r1_CURVE_PARAMETERS {
    .name = "P-256",
    .prime = "FFFFFFFF_00000001_00000000_00000000_00000000_FFFFFFFF_FFFFFFFF_FFFFFFFF"sv,
    .a = "FFFFFFFF_00000001_00000000_00000000_00000000_FFFFFFFF_FFFFFFFF_FFFFFFFC"sv,
    .b = "5AC635D8_AA3A93E7_B3EBBD55_769886BC_651D06B0_CC53B0F6_3BCE3C3E_27D2604B"sv,
    .order = "FFFFFFFF_00000000_FFFFFFFF_FFFFFFFF_BCE6FAAD_A7179E84_F3B9CAC2_FC632551"sv,
    .generator_point = "04_6B17D1F2_E12C4247_F8BCE6E5_63A440F2_77037D81_2DEB33A0_F4A13945_D898C296_4FE342E2_FE1A7F9B_8EE7EB4A_7C0F9E16_2BCE3357_6B315ECE_CBB64068_37BF51F5"sv,
};
using SECP256r1 = SECPxxxr1<256, SECP256r1_CURVE_PARAMETERS>;

// SECP384r1 curve
static constexpr SECPxxxr1CurveParameters SECP384r1_CURVE_PARAMETERS {
    .name = "P-384",
    .prime = "FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFE_FFFFFFFF_00000000_00000000_FFFFFFFF"sv,
    .a = "FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFE_FFFFFFFF_00000000_00000000_FFFFFFFC"sv,
    .b = "B3312FA7_E23EE7E4_988E056B_E3F82D19_181D9C6E_FE814112_0314088F_5013875A_C656398D_8A2ED19D_2A85C8ED_D3EC2AEF"sv,
    .order = "FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_C7634D81_F4372DDF_581A0DB2_48B0A77A_ECEC196A_CCC52973"sv,
    .generator_point = "04_AA87CA22_BE8B0537_8EB1C71E_F320AD74_6E1D3B62_8BA79B98_59F741E0_82542A38_5502F25D_BF55296C_3A545E38_72760AB7_3617DE4A_96262C6F_5D9E98BF_9292DC29_F8F41DBD_289A147C_E9DA3113_B5F0B8C0_0A60B1CE_1D7E819D_7A431D7C_90EA0E5F"sv,
};
using SECP384r1 = SECPxxxr1<384, SECP384r1_CURVE_PARAMETERS>;

// SECP521r1 curve
static constexpr SECPxxxr1CurveParameters SECP521r1_CURVE_PARAMETERS {
    .name = "P-521",
    .prime = "01FF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF"sv,
    .a = "01FF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFC"sv,
    .b = "0051_953EB961_8E1C9A1F_929A21A0_B68540EE_A2DA725B_99B315F3_B8B48991_8EF109E1_56193951_EC7E937B_1652C0BD_3BB1BF07_3573DF88_3D2C34F1_EF451FD4_6B503F00"sv,
    .order = "01FF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFF_FFFFFFFA_51868783_BF2F966B_7FCC0148_F709A5D0_3BB5C9B8_899C47AE_BB6FB71E_91386409"sv,
    .generator_point = "04_00C6_858E06B7_0404E9CD_9E3ECB66_2395B442_9C648139_053FB521_F828AF60_6B4D3DBA_A14B5E77_EFE75928_FE1DC127_A2FFA8DE_3348B3C1_856A429B_F97E7E31_C2E5BD66_0118_39296A78_9A3BC004_5C8A5FB4_2C7D1BD9_98F54449_579B4468_17AFBD17_273E662C_97EE7299_5EF42640_C550B901_3FAD0761_353C7086_A272C240_88BE9476_9FD16650"sv,
};
using SECP521r1 = SECPxxxr1<521, SECP521r1_CURVE_PARAMETERS>;

}
