#include "purple/purple_archive_crypto.h"
#include <limits>
#include <mutex>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

namespace Purple {

struct DesktopArchiveCrypto::Keys {
	std::mutex mutex;
	bool revoked = false;
	std::map<QString, QByteArray> values;
	~Keys() {
		for (auto &[id, key] : values) {
			DesktopArchiveCrypto::Clear(key);
		}
	}
};

DesktopArchiveCrypto::DesktopArchiveCrypto() : _keys(std::make_shared<Keys>()) {}
void DesktopArchiveCrypto::Revoke() {
	const auto lock = std::lock_guard(_keys->mutex);
	_keys->revoked = true;
	for (auto &[id, key] : _keys->values) {
		Clear(key);
	}
	_keys->values.clear();
}

void DesktopArchiveCrypto::Clear(QByteArray &bytes) {
	if (!bytes.isEmpty()) {
		OPENSSL_cleanse(bytes.data(), bytes.size());
	}
	bytes.clear();
}

bool DesktopArchiveCrypto::AddKey(const QString &id, const QByteArray &key) {
	const auto lock = std::lock_guard(_keys->mutex);
	if (_keys->revoked || id.isEmpty() || key.size() != kArchiveDataKeyBytes
		|| _keys->values.contains(id)) {
		return false;
	}
	_keys->values[id] = key;
	_keys->values[id].detach();
	return true;
}

std::optional<QByteArray> DesktopArchiveCrypto::Random(int size) {
	if (size < 1 || size > 4096) {
		return {};
	}
	auto result = QByteArray(size, char(0));
	return RAND_bytes(reinterpret_cast<unsigned char*>(result.data()), size) == 1
			   ? std::optional(result)
			   : std::nullopt;
}

std::optional<QByteArray> DesktopArchiveCrypto::Derive(
	const QByteArray &input, const ArchiveKeyDerivation &plan) {
	if (input.isEmpty() || input.size() > std::numeric_limits<int>::max()
		|| plan.salt.size() > std::numeric_limits<int>::max()
		|| plan.info.size() > std::numeric_limits<int>::max() || plan.outputBytes != 32) {
		return {};
	}
	const auto context = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>(
		EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
	if (!context) {
		return {};
	}
	auto result = QByteArray(32, char(0));
	auto size = size_t(result.size());
	const auto ok = EVP_PKEY_derive_init(context.get()) > 0
					&& EVP_PKEY_CTX_set_hkdf_md(context.get(), EVP_sha256()) > 0
					&& EVP_PKEY_CTX_set1_hkdf_salt(context.get(),
						   reinterpret_cast<const unsigned char*>(plan.salt.constData()),
						   int(plan.salt.size()))
						   > 0
					&& EVP_PKEY_CTX_set1_hkdf_key(context.get(),
						   reinterpret_cast<const unsigned char*>(input.constData()),
						   int(input.size()))
						   > 0
					&& EVP_PKEY_CTX_add1_hkdf_info(context.get(),
						   reinterpret_cast<const unsigned char*>(plan.info.constData()),
						   int(plan.info.size()))
						   > 0
					&& EVP_PKEY_derive(context.get(),
						   reinterpret_cast<unsigned char*>(result.data()), &size)
						   > 0
					&& size == 32;
	if (!ok) {
		Clear(result);
		return {};
	}
	return result;
}

std::optional<QByteArray> DesktopArchiveCrypto::Hmac(
	const QByteArray &key, const QByteArray &input) {
	if (key.size() != 32) {
		return {};
	}
	auto result = QByteArray(32, char(0));
	auto size = unsigned(0);
	if (!HMAC(EVP_sha256(), key.constData(), int(key.size()),
			reinterpret_cast<const unsigned char*>(input.constData()), input.size(),
			reinterpret_cast<unsigned char*>(result.data()), &size)
		|| size != 32) {
		Clear(result);
		return {};
	}
	return result;
}

auto DesktopArchiveCrypto::indexHmac(const QString &id) const
-> std::function<std::optional<QByteArray>(const QByteArray &)> {
	return [keys = _keys, id](const QByteArray &bytes) -> std::optional<QByteArray> {
		const auto lock = std::lock_guard(keys->mutex);
		const auto found = keys->values.find(id);
		return found == keys->values.end() ? std::nullopt : Hmac(found->second, bytes);
	};
}

ArchiveCrypto DesktopArchiveCrypto::callbacks() const {
	auto result = ArchiveCrypto();
	result.random = Random;
	result.sealAes256Gcm
		= [keys = _keys](const QString &id, const QByteArray &nonce,
			  const QByteArray &plain,
			  const QByteArray &aad) -> std::optional<ArchiveSealedBytes> {
		const auto lock = std::lock_guard(keys->mutex);
		const auto key = keys->values.find(id);
		if (key == keys->values.end() || nonce.size() != 12
			|| plain.size() > kArchiveEnvelopeByteLimit || aad.size() > 16384) {
			return {};
		}
		const auto context
			= std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>(
				EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
		if (!context) {
			return {};
		}
		auto sealed = ArchiveSealedBytes{
			QByteArray(plain.size() + 16, char(0)), QByteArray(16, char(0))};
		auto count = 0;
		auto written = 0;
		const auto ok
			= EVP_EncryptInit_ex(
				  context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr)
				  == 1
			  && EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr)
					 == 1
			  && EVP_EncryptInit_ex(context.get(), nullptr, nullptr,
					 reinterpret_cast<const unsigned char*>(key->second.constData()),
					 reinterpret_cast<const unsigned char*>(nonce.constData()))
					 == 1
			  && EVP_EncryptUpdate(context.get(), nullptr, &count,
					 reinterpret_cast<const unsigned char*>(aad.constData()),
					 int(aad.size()))
					 == 1
			  && EVP_EncryptUpdate(context.get(),
					 reinterpret_cast<unsigned char*>(sealed.ciphertext.data()), &written,
					 reinterpret_cast<const unsigned char*>(plain.constData()),
					 int(plain.size()))
					 == 1
			  && EVP_EncryptFinal_ex(context.get(),
					 reinterpret_cast<unsigned char*>(sealed.ciphertext.data()) + written,
					 &count)
					 == 1
			  && EVP_CIPHER_CTX_ctrl(
					 context.get(), EVP_CTRL_GCM_GET_TAG, 16, sealed.tag.data())
					 == 1;
		if (!ok) {
			return {};
		}
		sealed.ciphertext.resize(written + count);
		return sealed;
	};
	result.openAes256Gcm = [keys = _keys](const QString &id, const QByteArray &nonce,
							   const ArchiveSealedBytes &sealed,
							   const QByteArray &aad) -> std::optional<QByteArray> {
		const auto lock = std::lock_guard(keys->mutex);
		const auto key = keys->values.find(id);
		if (key == keys->values.end() || nonce.size() != 12 || sealed.tag.size() != 16
			|| sealed.ciphertext.size() > kArchiveEnvelopeByteLimit
			|| aad.size() > 16384) {
			return {};
		}
		const auto context
			= std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>(
				EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
		if (!context) {
			return {};
		}
		auto plain = QByteArray(sealed.ciphertext.size() + 16, char(0));
		auto count = 0;
		auto written = 0;
		const auto ok
			= EVP_DecryptInit_ex(
				  context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr)
				  == 1
			  && EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr)
					 == 1
			  && EVP_DecryptInit_ex(context.get(), nullptr, nullptr,
					 reinterpret_cast<const unsigned char*>(key->second.constData()),
					 reinterpret_cast<const unsigned char*>(nonce.constData()))
					 == 1
			  && EVP_DecryptUpdate(context.get(), nullptr, &count,
					 reinterpret_cast<const unsigned char*>(aad.constData()),
					 int(aad.size()))
					 == 1
			  && EVP_DecryptUpdate(context.get(),
					 reinterpret_cast<unsigned char*>(plain.data()), &written,
					 reinterpret_cast<const unsigned char*>(
						 sealed.ciphertext.constData()),
					 int(sealed.ciphertext.size()))
					 == 1
			  && EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, 16,
					 const_cast<char*>(sealed.tag.constData()))
					 == 1
			  && EVP_DecryptFinal_ex(context.get(),
					 reinterpret_cast<unsigned char*>(plain.data()) + written, &count)
					 == 1;
		if (!ok) {
			Clear(plain);
			return {};
		}
		plain.resize(written + count);
		return plain;
	};
	return result;
}

}
