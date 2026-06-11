using System.Security.Cryptography;

if (args.Length < 3)
{
    Console.Error.WriteLine("Usage: PayloadEncrypt <input.bin> <output.enc> <key-base64>");
    return 1;
}

var inputPath = args[0];
var outputPath = args[1];
var key = Convert.FromBase64String(args[2].Trim());
if (key.Length != 32)
{
    Console.Error.WriteLine("PAYLOAD_KEY must decode to 32 bytes.");
    return 1;
}

var plain = await File.ReadAllBytesAsync(inputPath);
var nonce = RandomNumberGenerator.GetBytes(12);
var cipher = new byte[plain.Length];
var tag = new byte[16];
using (var gcm = new AesGcm(key, 16))
    gcm.Encrypt(nonce, plain, cipher, tag);

await using var outStream = File.Create(outputPath);
await using var writer = new BinaryWriter(outStream);
writer.Write(0x454D5243u); // CRME
writer.Write(1u);
writer.Write(nonce);
writer.Write(cipher);
writer.Write(tag);
Console.WriteLine($"Encrypted {plain.Length} -> {outputPath} ({plain.Length + 32} bytes wire)");
return 0;
