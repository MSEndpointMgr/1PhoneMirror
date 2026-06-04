#!/usr/bin/env pwsh
# SRP-3072-SHA512 reference replay for AirPlay 2 HAP pair-setup.
# Reads a 1PhoneMirror log containing [HAP-SRP-DUMP] lines and recomputes
# what M1 SHOULD be per the SRP-6a / RFC 5054 spec.

param([Parameter(Mandatory=$true)] [string] $LogPath)

Add-Type -AssemblyName System.Numerics

$Nhex = `
"FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1" + `
"29024E088A67CC74020BBEA63B139B22514A08798E3404DD" + `
"EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245" + `
"E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED" + `
"EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3D" + `
"C2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F" + `
"83655D23DCA3AD961C62F356208552BB9ED529077096966D" + `
"670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B" + `
"E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9" + `
"DE2BCBF6955817183995497CEA956AE515D2261898FA0510" + `
"15728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64" + `
"ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7" + `
"ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6B" + `
"F12FFA06D98A0864D87602733EC86A64521F2B18177B200C" + `
"BBE117577A615D6C770988C0BAD946E208E24FA074E5AB31" + `
"43DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF"

$PrimeLen = 384

function HexToBytes([string]$h) {
    $h = $h -replace '\s',''
    $bytes = New-Object byte[] ($h.Length / 2)
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $bytes[$i] = [Convert]::ToByte($h.Substring($i*2, 2), 16)
    }
    return ,$bytes
}
function BytesToHex([byte[]]$b) { ($b | ForEach-Object { $_.ToString('x2') }) -join '' }

# BigInteger in .NET is little-endian signed. To parse big-endian unsigned hex,
# prefix "00" if MSB has high bit set; then reverse.
function HexToBigInt([string]$h) {
    $h = $h -replace '\s',''
    if ([Convert]::ToByte($h.Substring(0,2),16) -ge 0x80) { $h = "00" + $h }
    $b = HexToBytes $h
    [array]::Reverse($b)
    return [System.Numerics.BigInteger]::new($b)
}
function BigIntToPaddedBytes([System.Numerics.BigInteger]$n, [int]$len) {
    $b = $n.ToByteArray()  # little-endian, signed
    # strip trailing 0x00 sign byte if present
    while ($b.Length -gt 1 -and $b[$b.Length-1] -eq 0) {
        $b = $b[0..($b.Length-2)]
    }
    [array]::Reverse($b)  # now big-endian, minimal
    if ($b.Length -gt $len) { throw "value too big ($($b.Length) > $len)" }
    $out = New-Object byte[] $len
    [Array]::Copy($b, 0, $out, $len - $b.Length, $b.Length)
    return ,$out
}
function BigIntToMinimalBytes([System.Numerics.BigInteger]$n) {
    $b = $n.ToByteArray()
    while ($b.Length -gt 1 -and $b[$b.Length-1] -eq 0) {
        $b = $b[0..($b.Length-2)]
    }
    [array]::Reverse($b)
    return ,$b
}

$sha = [System.Security.Cryptography.SHA512]::Create()
function SHA512Bytes([byte[]]$data) { return ,$sha.ComputeHash($data) }
function SHA512Concat([byte[][]]$parts) {
    $total = 0; foreach ($p in $parts) { $total += $p.Length }
    $buf = New-Object byte[] $total
    $o = 0
    foreach ($p in $parts) { [Array]::Copy($p, 0, $buf, $o, $p.Length); $o += $p.Length }
    return ,$sha.ComputeHash($buf)
}
function XorBytes([byte[]]$a, [byte[]]$b) {
    $r = New-Object byte[] $a.Length
    for ($i = 0; $i -lt $a.Length; $i++) { $r[$i] = $a[$i] -bxor $b[$i] }
    return ,$r
}

$N    = HexToBigInt $Nhex
$Nmin = BigIntToMinimalBytes $N
$Npad = BigIntToPaddedBytes $N $PrimeLen
$g    = [System.Numerics.BigInteger]::new(5)
$gmin = [byte[]](,([byte]5))
$gpad = BigIntToPaddedBytes $g $PrimeLen

# Parse log into "captures" — one per pair-setup attempt.
$lines = Get-Content -LiteralPath $LogPath
$captures = New-Object System.Collections.ArrayList
$current = $null
foreach ($ln in $lines) {
    if ($ln -match 'pair-setup M2 \(server side\)') {
        if ($current) { [void]$captures.Add($current) }
        $current = @{}
    }
    if (-not $current) { continue }
    if ($ln -match 'I="([^"]*)"')              { $current.I = $matches[1] }
    if ($ln -match 'setup_code="([^"]*)"')     { $current.code = $matches[1] }
    if ($ln -match 'salt\(\d+\)=([0-9a-f]+)')      { $current.salt = HexToBytes $matches[1] }
    if ($ln -match 'B\(\d+\)=([0-9a-f]+)')         { $current.B    = HexToBytes $matches[1] }
    if ($ln -match 'v\(\d+\)=([0-9a-f]+)')         { $current.v    = HexToBytes $matches[1] }
    if ($ln -match 'A\(\d+\)=([0-9a-f]+)')         { $current.A    = HexToBytes $matches[1] }
    if ($ln -match 'S\(\d+\)=([0-9a-f]+)')         { $current.S    = HexToBytes $matches[1] }
    if ($ln -match 'K\(\d+\)=([0-9a-f]+)')         { $current.K    = HexToBytes $matches[1] }
    if ($ln -match 'M1_server\(\d+\)=([0-9a-f]+)') { $current.Msv  = HexToBytes $matches[1] }
    if ($ln -match 'M1_client\(\d+\)=([0-9a-f]+)') { $current.Mcl  = HexToBytes $matches[1] }
}
if ($current) { [void]$captures.Add($current) }

Write-Host "Parsed $($captures.Count) capture(s) from $LogPath"
Write-Host ""

$idx = 0
foreach ($c in $captures) {
    $idx++
    Write-Host "============ Capture #$idx ============"
    Write-Host "I            = $($c.I)"
    Write-Host "setup_code   = $($c.code)"
    Write-Host "salt         = $(BytesToHex $c.salt)"
    Write-Host "|A|=$($c.A.Length)B |B|=$($c.B.Length)B |S|=$($c.S.Length)B |K|=$($c.K.Length)B"
    Write-Host ""

    # 1. K reference: K_ref = SHA512(PAD(S))
    $K_ref = SHA512Bytes $c.S
    $K_ok = (BytesToHex $K_ref) -eq (BytesToHex $c.K)
    Write-Host ("K (server) == SHA512(PAD(S)) ?  {0}" -f $K_ok)
    if (-not $K_ok) {
        Write-Host "  server K: $(BytesToHex $c.K)"
        Write-Host "  ref    K: $(BytesToHex $K_ref)"
    }

    # 2. Compute v_ref from (I, password, salt) and compare to server v
    $ip = [System.Text.Encoding]::UTF8.GetBytes("$($c.I):$($c.code)")
    $inner = SHA512Bytes $ip
    $xbytes = SHA512Concat @($c.salt, $inner)
    $x = HexToBigInt (BytesToHex $xbytes)
    $v = [System.Numerics.BigInteger]::ModPow($g, $x, $N)
    $vref = BigIntToPaddedBytes $v $PrimeLen
    $v_ok = (BytesToHex $vref) -eq (BytesToHex $c.v)
    Write-Host ("v == g^x mod N (with x = H(salt|H(I:p))) ?  {0}" -f $v_ok)
    if (-not $v_ok) {
        Write-Host "  server v[0..16]: $((BytesToHex $c.v).Substring(0,32))"
        Write-Host "  ref    v[0..16]: $((BytesToHex $vref).Substring(0,32))"
    }

    # 3. Compute M1 in 4 variants of H(N), H(g) padding.
    $hN_min = SHA512Bytes $Nmin
    $hN_pad = SHA512Bytes $Npad
    $hg_min = SHA512Bytes $gmin
    $hg_pad = SHA512Bytes $gpad
    $hI = SHA512Bytes ([System.Text.Encoding]::UTF8.GetBytes($c.I))

    Write-Host ""
    Write-Host "M1 candidate values (first 16 bytes):"
    foreach ($nv in @(@{n="Nmin";h=$hN_min}, @{n="Npad";h=$hN_pad})) {
        foreach ($gv in @(@{n="gmin";h=$hg_min}, @{n="gpad";h=$hg_pad})) {
            $xor = XorBytes $nv.h $gv.h
            $m1 = SHA512Concat @($xor, $hI, $c.salt, $c.A, $c.B, $c.K)
            $tag = ""
            if ((BytesToHex $m1) -eq (BytesToHex $c.Msv)) { $tag += "  <-MATCHES SERVER" }
            if ((BytesToHex $m1) -eq (BytesToHex $c.Mcl)) { $tag += "  <-MATCHES CLIENT" }
            Write-Host ("  {0}+{1}  = {2}{3}" -f $nv.n, $gv.n, (BytesToHex $m1).Substring(0,32), $tag)
        }
    }
    Write-Host ("  server M1[0..16] = {0}" -f (BytesToHex $c.Msv).Substring(0,32))
    Write-Host ("  client M1[0..16] = {0}" -f (BytesToHex $c.Mcl).Substring(0,32))

    # 4. Try alternate (I, password) variants against client M1 to find what iOS used.
    Write-Host ""
    Write-Host "=== brute force I / password variants vs client M1 ==="
    $pwClean = $c.code.Replace("-","")
    $tries = @(
        @{I="Pair-Setup"; pw=$c.code},
        @{I=""          ; pw=$c.code},
        @{I="Pair-Setup"; pw=$pwClean},
        @{I=""          ; pw=$pwClean},
        @{I="admin"     ; pw=$c.code},
        @{I="Pair-Setup"; pw=$c.code.ToLower()}
    )
    $found = $false
    foreach ($t in $tries) {
        $ip2 = [System.Text.Encoding]::UTF8.GetBytes("$($t.I):$($t.pw)")
        $inner2 = SHA512Bytes $ip2
        $xb2 = SHA512Concat @($c.salt, $inner2)
        $x2 = HexToBigInt (BytesToHex $xb2)
        $v2 = [System.Numerics.BigInteger]::ModPow($g, $x2, $N)
        # Client M1 requires recomputing S too — but we use server's K (which came from server's S using server's b).
        # If iOS used a different password, our K would already be wrong above. So just check if any (I,pw) makes M1 match
        # against the captured K (it shouldn't unless iOS shares K).
        $hI2 = SHA512Bytes ([System.Text.Encoding]::UTF8.GetBytes($t.I))
        foreach ($nv in @(@{n="Nmin";h=$hN_min}, @{n="Npad";h=$hN_pad})) {
            foreach ($gv in @(@{n="gmin";h=$hg_min}, @{n="gpad";h=$hg_pad})) {
                $xor = XorBytes $nv.h $gv.h
                $m1 = SHA512Concat @($xor, $hI2, $c.salt, $c.A, $c.B, $c.K)
                if ((BytesToHex $m1) -eq (BytesToHex $c.Mcl)) {
                    Write-Host ("  MATCH! I={0}  pw={1}  variant={2}+{3}" -f $t.I, $t.pw, $nv.n, $gv.n)
                    $found = $true
                }
            }
        }
    }
    if (-not $found) {
        Write-Host "  no match — iPhone is hashing M1 over different bytes (I/pw/structure)."
    }
    Write-Host ""
}
