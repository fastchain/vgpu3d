function Exports($p) {
  $b=[IO.File]::ReadAllBytes($p); $pe=[BitConverter]::ToInt32($b,0x3c); $n=[BitConverter]::ToUInt16($b,$pe+6); $opt=[BitConverter]::ToUInt16($b,$pe+20)
  $secs=@(); $s=$pe+24+$opt
  for($i=0;$i -lt $n;$i++){ $o=$s+40*$i; $secs+=,@([BitConverter]::ToUInt32($b,$o+12),[BitConverter]::ToUInt32($b,$o+8),[BitConverter]::ToUInt32($b,$o+20)) }
  function R2O($r){ foreach($x in $secs){ if($r -ge $x[0] -and $r -lt $x[0]+$x[1]){ return $r-$x[0]+$x[2] } }; return -1 }
  $ed=[BitConverter]::ToUInt32($b,$pe+24+112); $eo=R2O $ed
  $cnt=[BitConverter]::ToUInt32($b,$eo+24); $names=R2O ([BitConverter]::ToUInt32($b,$eo+32))
  $h=@{}; for($i=0;$i -lt $cnt;$i++){ $no=R2O ([BitConverter]::ToUInt32($b,$names+4*$i)); $e=$no; while($b[$e] -ne 0){$e++}; $h[[Text.Encoding]::ASCII.GetString($b,$no,$e-$no)]=1 }
  return $h
}
$nt=Exports C:\Windows\System32\ntoskrnl.exe; $hal=Exports C:\Windows\System32\hal.dll
"ntoskrnl exports: $($nt.Count)"
foreach($l in Get-Content C:\vgpu\imports.txt){ $d,$f=$l.Split(' '); $h= if($d -match 'HAL'){$hal}else{$nt}; if(-not $h.ContainsKey($f)){ "MISSING $d $f" } }
'done'
