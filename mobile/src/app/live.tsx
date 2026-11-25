import Constants, { ExecutionEnvironment } from 'expo-constants';
import { File, Paths } from 'expo-file-system';
import { isAvailableAsync, shareAsync } from 'expo-sharing';
import { useEffect, useRef, useState } from 'react';
import { Platform, Pressable, ScrollView, StyleSheet, View } from 'react-native';
import { SafeAreaView } from 'react-native-safe-area-context';

import { ThemedText } from '@/components/themed-text';
import { ThemedView } from '@/components/themed-view';
import { BottomTabInset, MaxContentWidth, Spacing } from '@/constants/theme';
import { connectRelay } from '@/telemetry/connectRelay';
import { formatCapture, type ReceivedTelemetry } from '@/telemetry/relayStream';
import type { FootPeakTelemetry, StatsTelemetry } from '@/telemetry/types';

type Capture = { startedAt: number; rows: ReceivedTelemetry[] };
type CaptureStatus = 'idle' | 'recording' | 'saving' | 'retry';

async function writeCapture(capture: Capture) {
  const stamp = new Date(capture.startedAt).toISOString().replace(/[:.]/g, '-');
  const file = new File(Paths.document, `dragonsense-live-${stamp}.ndjson`);
  file.create({ overwrite: true });
  await file.write(formatCapture(capture.rows));
  return file;
}

function Action({ label, onPress, disabled = false }: {
  label: string;
  onPress: () => void;
  disabled?: boolean;
}) {
  return (
    <Pressable accessibilityRole="button" disabled={disabled} onPress={onPress}
      style={({ pressed }) => ({ opacity: pressed || disabled ? 0.5 : 1 })}>
      <ThemedView type="backgroundSelected" style={styles.action}>
        <ThemedText type="smallBold">{label}</ThemedText>
      </ThemedView>
    </Pressable>
  );
}

function Reading({ label, load }: { label: string; load?: number }) {
  return (
    <View style={styles.reading}>
      <ThemedText themeColor="textSecondary">{label}</ThemedText>
      <ThemedText type="smallBold">{load === undefined ? '—' : `${load.toFixed(1)} lb`}</ThemedText>
    </View>
  );
}

export default function LiveTestScreen() {
  const [status, setStatus] = useState('Connect to the Freenove to receive live readings.');
  const [link, setLink] = useState<'idle' | 'connecting' | 'connected' | 'closing'>('idle');
  const [stats, setStats] = useState<StatsTelemetry>();
  const [peak, setPeak] = useState<FootPeakTelemetry['loads']>(null);
  const [captureStatus, setCaptureStatus] = useState<CaptureStatus>('idle');
  const [samples, setSamples] = useState(0);
  const [savedFile, setSavedFile] = useState<File>();
  const [fileError, setFileError] = useState('');
  const connection = useRef<ReturnType<typeof connectRelay> | null>(null);
  const recording = useRef<Capture | null>(null);
  const pendingSave = useRef<Capture | null>(null);
  const saving = useRef(false);

  useEffect(() => () => {
    void connection.current?.close().catch(() => undefined);
    const unsaved = recording.current ?? pendingSave.current;
    if (unsaved && !saving.current) void writeCapture(unsaved).catch(console.error);
  }, []);

  const canUseBluetooth = (Platform.OS === 'ios' || Platform.OS === 'android') &&
    Constants.executionEnvironment !== ExecutionEnvironment.StoreClient;

  async function saveCapture() {
    const capture = recording.current ?? pendingSave.current;
    if (!capture || saving.current) return;
    recording.current = null;
    pendingSave.current = capture;
    saving.current = true;
    setCaptureStatus('saving');
    setFileError('');
    try {
      setSavedFile(await writeCapture(capture));
      pendingSave.current = null;
      setCaptureStatus('idle');
    } catch (error) {
      setFileError(error instanceof Error ? error.message : 'Could not save the capture.');
      setCaptureStatus('retry');
    } finally {
      saving.current = false;
    }
  }

  async function disconnect(message = 'Disconnected.') {
    const active = connection.current;
    if (!active) return;
    setLink('closing');
    setStatus(message);
    setStats(undefined);
    setPeak(null);
    if (recording.current) void saveCapture();
    try {
      await active.close();
    } catch (error) {
      setStatus(error instanceof Error ? error.message : 'Bluetooth cleanup failed.');
    } finally {
      connection.current = null;
      setLink('idle');
    }
  }

  function connect() {
    if (connection.current) return;
    setLink('connecting');
    setStatus('Starting Bluetooth…');
    connection.current = connectRelay({
      onStatus: setStatus,
      onError: (message) => void disconnect(message),
      onData(records) {
        for (const { message } of records) {
          if (message.type === 'stats') {
            setStats(message);
            setLink('connected');
            setStatus('Receiving live data from the Freenove.');
          } else if (message.type === 'foot_peak') {
            setPeak(message.loads);
          }
        }
        const capture = recording.current;
        if (capture) {
          capture.rows.push(...records);
          setSamples(capture.rows.length);
        }
      },
    });
  }

  function startCapture() {
    if (recording.current || pendingSave.current) return;
    recording.current = { startedAt: Date.now(), rows: [] };
    setSamples(0);
    setSavedFile(undefined);
    setFileError('');
    setCaptureStatus('recording');
  }

  async function shareCapture() {
    if (!savedFile) return;
    try {
      if (!(await isAvailableAsync())) throw new Error('Sharing is unavailable on this phone.');
      await shareAsync(savedFile.uri, { mimeType: 'application/x-ndjson', dialogTitle: 'Share PERG capture' });
    } catch (error) {
      setFileError(error instanceof Error ? error.message : 'Could not share the capture.');
    }
  }

  return (
    <ThemedView style={styles.fill}>
      <SafeAreaView style={styles.safeArea} edges={['top', 'left', 'right']}>
        <ScrollView contentContainerStyle={styles.content}>
          <ThemedText type="subtitle">Live Test</ThemedText>
          <ThemedText themeColor="textSecondary">
            {canUseBluetooth ? status : 'Open DragonSense in a native development build to test Bluetooth.'}
          </ThemedText>
          {canUseBluetooth && (
            <Action
              label={link === 'idle' ? 'Connect to Freenove' : link === 'connecting' ? 'Cancel connection' : 'Disconnect'}
              onPress={link === 'idle' ? connect : () => void disconnect()}
              disabled={link === 'closing'}
            />
          )}

          <ThemedView type="backgroundElement" style={styles.panel}>
            <ThemedText type="smallBold">Foot force</ThemedText>
            <Reading label="Left top" load={stats?.footLoads?.leftTop} />
            <Reading label="Left bottom" load={stats?.footLoads?.leftBottom} />
            <Reading label="Right top" load={stats?.footLoads?.rightTop} />
            <Reading label="Right bottom" load={stats?.footLoads?.rightBottom} />
          </ThemedView>
          <ThemedView type="backgroundElement" style={styles.panel}>
            <Reading label="Last stroke peak" load={peak ? peak.left + peak.right : undefined} />
            <Reading label="Left at peak" load={peak?.left} />
            <Reading label="Right at peak" load={peak?.right} />
          </ThemedView>

          {canUseBluetooth && (
            <>
              {captureStatus === 'idle' && <Action label="Start capture" onPress={startCapture} disabled={link !== 'connected'} />}
              {captureStatus === 'recording' && <Action label="Stop and save capture" onPress={() => void saveCapture()} />}
              {captureStatus === 'retry' && <Action label="Retry saving capture" onPress={() => void saveCapture()} />}
              {captureStatus === 'saving' && <ThemedText>Saving capture…</ThemedText>}
              {(captureStatus === 'recording' || savedFile) && (
                <ThemedText themeColor="textSecondary">{samples} messages captured</ThemedText>
              )}
              {savedFile && <Action label="Share saved capture" onPress={() => void shareCapture()} />}
              {fileError !== '' && <ThemedText>{fileError}</ThemedText>}
            </>
          )}
        </ScrollView>
      </SafeAreaView>
    </ThemedView>
  );
}

const styles = StyleSheet.create({
  fill: { flex: 1 },
  safeArea: { flex: 1, width: '100%', maxWidth: MaxContentWidth, alignSelf: 'center' },
  content: { padding: Spacing.four, paddingBottom: BottomTabInset + Spacing.four, gap: Spacing.three },
  action: { borderRadius: Spacing.three, minHeight: 48, alignItems: 'center', justifyContent: 'center' },
  panel: { padding: Spacing.three, borderRadius: Spacing.three, gap: Spacing.two },
  reading: { flexDirection: 'row', justifyContent: 'space-between', gap: Spacing.two },
});
